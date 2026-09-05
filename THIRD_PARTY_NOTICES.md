# Third-party notices

BlackBox uses SDL 3 (zlib license), Dear ImGui and ImPlot (MIT licenses), and
SQLite (public domain). Their original notices, plus those of resolved build
dependencies, are included under `licenses/<package>/copyright` in installed
packages. Catch2 is used by development tests and is not linked into the app.
The package notices are copied from the dependency versions resolved by the
pinned vcpkg baseline in `vcpkg.json`; this file does not replace those notices.

Operating-system fonts are loaded from the user's installation. BlackBox does
not redistribute those fonts. System libraries remain subject to their own
platform terms.

BlackBox's own source and binaries use the separate [project license](LICENSE.txt).
