# Fallback license texts

License files for bundled dependencies whose package ships none: Homebrew's
wxwidgets and boost kegs, and some MSYS2 packages. Each folder is named after
the Homebrew formula or the MSYS2 package without its prefix. The packaging
scripts copy a folder into a package's `licenses/` only when the package's own
copy is missing.
