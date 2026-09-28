# Rufus++ website

This directory contains the static Rufus++ project website. It has no runtime
or build dependencies.

Preview it from the repository root with:

```console
python3 -m http.server 4173 --directory website
```

Then open <http://127.0.0.1:4173/>.

The download and changelog sections query public, published GitHub Releases at
runtime. Draft entries are excluded; published prereleases remain visible so
development builds can be distributed transparently.

The `Deploy Website` GitHub Actions workflow publishes this directory to
<https://farrell236.github.io/rufuspp/> after changes reach `master`. It can
also be dispatched manually from the repository's **Actions** tab.
