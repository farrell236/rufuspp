# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded read-only FAT32 file comparison, independent of the native writer.

Compare every original file's short path, LFN records, size and SHA-256 after
the host has mounted FAT32. Extra host files and FAT allocation timestamps
are recorded separately from the original media files.
"""
import hashlib
import struct

from image_write_integration import require


class Fat32:
    def __init__(self,read,geometry):
        self.read_raw=read;self.base=geometry['fatOffset'];self.size=geometry['fatBytes']
        boot=self.read(self.base,512)
        require(boot[82:90]==b'FAT32   ' and struct.unpack_from('<H',boot,11)[0]==512,'expected FAT32/512')
        self.cluster_bytes=boot[13]*512
        require(512<=self.cluster_bytes<=65536 and self.cluster_bytes&(self.cluster_bytes-1)==0,
                'invalid FAT cluster size')
        reserved=struct.unpack_from('<H',boot,14)[0]
        sectors=struct.unpack_from('<I',boot,36)[0]
        require(reserved>0 and boot[16] in (1,2) and 0<sectors*512<=64*1024**2,'invalid bounded FAT geometry')
        self.data=self.base+(reserved+boot[16]*sectors)*512
        self.clusters=(self.base+self.size-self.data)//self.cluster_bytes
        require(0<self.clusters<(sectors*512)//4,'FAT table does not cover its data region')
        self.table=self.read(self.base+reserved*512,sectors*512)
        self.root=struct.unpack_from('<I',boot,44)[0]

    def read(self,offset,count):
        require(self.base<=offset and count>0 and count<=self.base+self.size-offset,
                'FAT read outside partition')
        data=self.read_raw(offset,count)
        require(len(data)==count,'short FAT readback');return data

    def chain(self,cluster,max_bytes):
        seen=set();result=[]
        while cluster<0x0ffffff8:
            require(2<=cluster<self.clusters+2 and cluster not in seen
                    and len(result)*self.cluster_bytes<max_bytes,'invalid, looping or excessive FAT chain')
            seen.add(cluster);result.append(cluster)
            cluster=struct.unpack_from('<I',self.table,cluster*4)[0]&0x0fffffff
        return result

    def blocks(self,clusters,size):
        index=0;remaining=size
        while remaining:
            first=clusters[index];count=1
            while (index+count<len(clusters) and clusters[index+count]==first+count
                   and count*self.cluster_bytes<4*1024**2):count+=1
            raw=self.read(self.data+(first-2)*self.cluster_bytes,count*self.cluster_bytes)
            yield raw[:remaining]
            remaining-=min(remaining,len(raw));index+=count

    def catalog(self):
        files={};directories=set();self.directory_names={}
        def walk(cluster,path):
            require(cluster not in directories and len(path)<32 and len(directories)<10000,
                    'cyclic or excessive FAT directory tree')
            directories.add(cluster);pending=[];names=set()
            clusters=self.chain(cluster,16*1024**2)
            for block in self.blocks(clusters,len(clusters)*self.cluster_bytes):
                for offset in range(0,len(block),32):
                    entry=block[offset:offset+32]
                    if entry[0]==0:return
                    if entry[0]==0xe5:pending=[];continue
                    if entry[11]==0x0f:pending.append(entry);continue
                    name=entry[:11];lfn=tuple(pending);pending=[]
                    if entry[11]&8 or name in (b'.          ',b'..         '):continue
                    require(name not in names,'duplicate FAT directory name');names.add(name)
                    child=(struct.unpack_from('<H',entry,20)[0]<<16)|struct.unpack_from('<H',entry,26)[0]
                    key=path+(name,)
                    if entry[11]&0x10:
                        self.directory_names[key]=lfn
                        walk(child,key)
                    else:
                        require(len(files)<100000,'excessive FAT files')
                        files[key]={'cluster':child,'size':struct.unpack_from('<I',entry,28)[0],'lfn':lfn}
        walk(self.root,())
        return files

    def checksum(self,entry):
        size=entry['size'];sha=hashlib.sha256()
        if size:
            clusters=self.chain(entry['cluster'],((size+self.cluster_bytes-1)//self.cluster_bytes)*self.cluster_bytes)
            require(len(clusters)==(size+self.cluster_bytes-1)//self.cluster_bytes,'short FAT file allocation')
            for block in self.blocks(clusters,size):sha.update(block)
        return sha.hexdigest()


def compare_files(original,written):
    expected,actual=original.catalog(),written.catalog()
    require(all(written.directory_names.get(path)==lfn for path,lfn in original.directory_names.items()),
            'original FAT directory long name changed or directory missing')
    checked=total=0
    for path,entry in expected.items():
        require(path in actual,'original media file missing: '+repr(path))
        other=actual[path]
        require(entry['size']==other['size'] and entry['lfn']==other['lfn'],
                'original file size or long name changed: '+repr(path))
        require(original.checksum(entry)==written.checksum(other),'original file content differs: '+repr(path))
        checked+=1;total+=entry['size']
        if checked%200==0:print('Independently checked {} staged files ({} bytes).'.format(checked,total),flush=True)
    return {'independentStagedFilesMatched':True,'independentStagedFilesChecked':checked,
            'independentStagedFileBytesChecked':total,'hostAddedFiles':len(actual.keys()-expected.keys())}
