set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# MSVC embeds STL annotation compatibility records in every C++ object. All
# dependencies in an ASan graph must therefore use the same instrumentation as
# BlackBox; mixing the ordinary x64-windows cache with instrumented objects is
# rejected by the linker and would leave third-party boundaries unobserved.
set(VCPKG_C_FLAGS "/fsanitize=address /Zi")
set(VCPKG_CXX_FLAGS "/fsanitize=address /Zi")
set(VCPKG_LINKER_FLAGS "/INCREMENTAL:NO")
