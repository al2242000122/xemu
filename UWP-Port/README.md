# xemu UWP Port

This directory contains the Universal Windows Platform host used to run xemu as
an embedded DLL on Windows and Xbox in Developer Mode. It owns the application
lifecycle, brokered file access, controller input, settings, logs, and the XAML
render surface.

The graphics path is:

```text
xemu NV2A -> OpenGL -> Mesa Gallium D3D12 -> D3D12/DXGI -> SwapChainPanel
```

The UWP host does not use Vulkan. SDL3 supplies the UWP platform and input
integration. Mesa supplies OpenGL through the Gallium D3D12 driver.

## Requirements

- Windows 10 or Windows 11
- Visual Studio with the Desktop C++ and Universal Windows Platform workloads
- Windows SDK 10.0.26100.0 or a compatible installed SDK
- MSYS2 UCRT64 with the runtime dependencies required by xemu
- A UWP-compatible SDL3 build
- The xemu UWP DLL and Mesa UWP OpenGL binaries
- Developer Mode enabled on the target PC or Xbox

Only the x64 package is currently configured and tested.

## Expected build outputs

Before building the host, prepare these artifacts:

```text
build-uwp-embed/qemu-system-i386.dll
build-uwp/mesa/src/gallium/targets/libgl-gdi/opengl32.dll
build-uwp/mesa/src/gallium/targets/wgl/gallium_wgl.dll
```

The SDL3 include directory, SDL3 runtime, and MSYS2 UCRT64 runtime directory
must also be configured in `UWP-Port.vcxproj`. Use MSBuild properties or paths
appropriate for the local development environment. Do not copy development
machine paths into distributable documentation or packages.

The packaged application must contain at least:

```text
qemu-system-i386.dll
opengl32.dll
gallium_wgl.dll
SDL3.dll
dxil.dll
libslirp-0.dll
```

It must also contain the transitive runtime DLLs referenced by these binaries.

## Build xemu as an embeddable DLL

Run the following commands from the repository root in an MSYS2 UCRT64 shell:

```sh
mkdir -p build-uwp-embed
cd build-uwp-embed
../configure \
  --target-list=i386-softmmu \
  --enable-uwp \
  --enable-sdl \
  --enable-opengl \
  --disable-vulkan \
  --disable-llvm
ninja
```

The result required by the host is `build-uwp-embed/qemu-system-i386.dll`.
Configuration may require additional dependency switches according to the
installed MSYS2 packages.

## Build SDL3 and Mesa

Build SDL3 for UWP x64 in Release mode. Configure the host project to use the
resulting SDL3 include directory and `SDL3.dll`.

Build Mesa for Windows/UWP x64 with:

- OpenGL enabled
- Gallium D3D12 graphics enabled
- Gallium D3D12 video enabled
- Vulkan drivers disabled
- LLVM disabled when the internal DXIL compiler is available

Place or configure the Mesa outputs so the host project can package
`opengl32.dll` and `gallium_wgl.dll` from the expected build tree.

## Build and package UWP-Port

Open `UWP-Port.vcxproj` in Visual Studio, select `Release` and `x64`, and build
the project. To create a sideload package from a Developer PowerShell for Visual
Studio, run:

```powershell
msbuild .\UWP-Port\UWP-Port.vcxproj `
  /t:Rebuild `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /p:AppxBundle=Never `
  /p:UapAppxPackageBuildMode=SideloadOnly `
  /m
```

The signed MSIX, certificate, symbols, and dependency packages are generated
under:

```text
UWP-Port/AppPackages/UWP-Port/<package-version>_x64_Test/
```

Increment the four-part `Identity Version` in `Package.appxmanifest` before
creating an update for an already installed PC or Xbox package.

## Runtime setup

On the Files page, select:

- Xbox BIOS/flash ROM
- MCPX boot ROM
- Xbox hard disk image
- DVD/XISO image

The Start xemu button is enabled only after all required files are available.
Files and folders are retained through the UWP Future Access List, so xemu uses
brokered virtual paths instead of unrestricted desktop filesystem paths.

Optional screenshot, games, and Memory Unit locations are configured on the
Storage page. Emulator settings are saved automatically. Network settings are
committed with the Save settings button on the Network page.

## Logs

Runtime diagnostics are written to:

```text
ApplicationData/LocalFolder/xemu.log
```

The Logs page displays and refreshes the latest 256 KB of this file. On a PC,
the file can also be retrieved from the installed package's `LocalState`
directory. On Xbox, use Device Portal to access application files and download
the log.

## UWP limitations

- The application must not terminate the process through `exit()` or `abort()`;
  failures are returned through the embedding API and log callback.
- Desktop-only Win32 APIs and libraries must not be introduced into packaged
  binaries.
- File access outside application storage must use brokered `StorageFile` or
  `StorageFolder` objects.
- PCAP is unavailable in this UWP build. NAT and UDP tunnel are the supported
  network backends.
- Rendering must remain attached to the XAML `SwapChainPanel`; desktop window
  ownership and desktop DXGI debug interfaces are not available on Xbox retail
  environments.

## Legal notice

No copyrighted Xbox firmware, MCPX ROM, hard disk image, game image, keys, or
other proprietary content is included. Users must provide files they are
legally entitled to use.
