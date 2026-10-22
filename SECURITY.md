# Security policy

This project is a development prototype and must not yet be used to write
production media. Guarded DD and staged ISO transports exist on macOS, Linux,
and Windows, and Windows also exposes FFU application through DISM. Every path
revalidates the source and whole removable target before destructive I/O, but
the transports have not completed removable-hardware fault testing.

Ordinary unsigned macOS development builds keep physical writes disabled.
The explicit `RUFUSPP_MACOS_UNSIGNED_ROOT_MODE` development variant omits the
helper and allows guarded device operations only when the entire application
is launched with an effective UID of zero. Its indicator reports UNPRIVILEGED
or ELEVATED; this mode is for controlled hardware testing and is not a production
privilege boundary. Signed builds delegate
destructive I/O to a narrow `SMAppService` launch daemon that mutually
authenticates the app, accepts an already-open source descriptor, and
independently revalidates the target. Linux and Windows currently perform raw
I/O in an explicitly elevated application process; moving those handles into
equivalent least-privilege helpers remains release hardening. Do not use any
development build against media containing important data.

Please report suspected vulnerabilities privately through this repository's
security-advisory feature. Do not send reports for this fork to the upstream
Rufus maintainer unless the issue also affects the upstream project.

Ordinary defects and feature requests should use the repository issue tracker.
Please include the operating system, architecture, Qt version, build type, and
steps needed to reproduce the problem.
