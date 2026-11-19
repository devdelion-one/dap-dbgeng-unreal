# dap-dbgeng (Unreal fork)

> **This is a fork of [svnscha/dap-dbgeng](https://github.com/svnscha/dap-dbgeng)** (v0.3.0), made for the
> debugger in Amethyst. It keeps the MIT licence and the original
> copyright. What it changes, all checked against Unreal Engine 5.7:
>
> - **Unreal Editor starts and runs under it.** First-chance exceptions and Unreal's thread-name signal go back to the
>   program instead of stopping it, and only a break-in the client asked for stops.
> - **The program survives its debugger.** It detaches when the adapter exits (`DEBUG_PROCESS_DETACH_ON_EXIT`), and
>   ends the program only when a `disconnect` asks for it.
> - **Breakpoints in modules that aren't loaded yet** wait for the module (`sourceModules`: source folder = module
>   name), then bind and say so with a `breakpoint` event. A line with no code moves to the next line that has some.
> - **A sturdier event loop.** It survives a failed wait, a continue right after a step, and stray engine breaks.
> - **Fast locals** (one read per level; lazy children), **hover** (locals and `this` members only, never a command),
>   and **thread names** from Windows.
> - **Unreal types read from memory** with the program's own PDB layouts: `FName`, `FString`, `FText`, `TArray`
>   items, `TMap` items as `[Key] = Value` and `TSet` items (read when opened), `UObject*` / `TObjectPtr` /
>   `TWeakObjectPtr` as "Name (Class)" (a dead weak pointer as `stale`), `TSharedPtr` / `TSharedRef` /
>   `TUniquePtr` (opening onto the object's real class, from its vtable), `TOptional`, `FGuid`, soft object paths,
>   gameplay tags and small number structs. A type's members are read once through dbghelp, since dbgeng takes up to
>   half a second per member query on a big template.
> - **An "ensure() failures" exception filter** (on by default). Off, a `__debugbreak` in the program's own code
>   (Unreal's `ensure()`) is passed over and the program runs on. On, the stop says which ensure failed.
> - **Set next statement** (`gotoTargets` / `goto`). A line resolves as a line breakpoint does, in the current
>   function only: a line outside it, or the function's own entry, is refused with the reason. `gotoTargets` takes
>   a `threadId` (an extension); `goto` moves that thread's instruction pointer, then reports a `goto` stop.
> - **Blueprint frames in the call stack.** Each function the engine's script interpreter runs (the thread's
>   `GTopTrackingStackFrame` chain of `FFrame`s) sits above the engine frame that runs it, named
>   "Blueprint · Function", with `moduleId: "Blueprint"` and a source that names the object (no file). Its scope
>   holds the function's parameters and locals, read through its own `FProperty` list, and `self`.
> - **Program output that isn't UTF-8** (a log line with an em dash, in the ANSI code page) is converted before it
>   is sent. Before, it ended the adapter.
>
> Releases here hold `dap-dbgeng.exe` and `LICENSE` only. It needs Microsoft's `dbgeng.dll`, `dbgmodel.dll`,
> `dbghelp.dll`, `dbgcore.dll` and `msdia140.dll` next to it, from the
> [Microsoft.Debugging.Platform.DbgEng](https://www.nuget.org/packages/Microsoft.Debugging.Platform.DbgEng) package
> (Amethyst downloads that package itself, after its licence is accepted).

The original README follows.

[![VS Marketplace](https://vsmarketplacebadges.dev/version/svnscha.vscode-dap-dbgeng.svg)](https://marketplace.visualstudio.com/items?itemName=svnscha.vscode-dap-dbgeng)
[![CI](https://github.com/svnscha/dap-dbgeng/actions/workflows/ci.yml/badge.svg)](https://github.com/svnscha/dap-dbgeng/actions/workflows/ci.yml)
[![Docs](https://github.com/svnscha/dap-dbgeng/actions/workflows/pages.yml/badge.svg)](https://svnscha.github.io/dap-dbgeng/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/platform-Windows-0078D6)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)

A Windows [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/) server
backed by the Windows debug engine (`dbgeng` / `dbghelp`). It lets DAP clients such as VS Code
debug native Windows user-mode processes (launch, attach, remote attach) and kernel targets.
C++20, CMake + Ninja, vcpkg manifest mode. Windows-only.

## Highlights

- **Launch, attach, remote (`dbgsrv`), and kernel** debugging through one adapter.
- **Breakpoints** (line + conditional), **stepping** (over/into/out, instruction-level),
  **call stack**, **variables/scopes/registers**, **set variable**, **disassembly view**, and
  **expression evaluation** via the engine.
- **Native DbgEng APIs** throughout - no fragile text scraping of WinDbg output.
- A built-in **session-trace recorder** that doubles as the project's replay-test format.

See [Supported features](docs/reference/features.md) for the full, honest list of what is and
isn't advertised.

## Documentation

Published at **[svnscha.github.io/dap-dbgeng](https://svnscha.github.io/dap-dbgeng/)** from
[`docs/`](docs) on every push to `main`. Build it locally with `npm run docs` (live preview) or
`npm run docs:build`:

- [Getting started](docs/getting-started.md)
- Scenarios: [local](docs/scenarios/local-debugging.md) ·
  [attach](docs/scenarios/attach.md) · [remote](docs/scenarios/remote-debugging.md) ·
  [driver](docs/scenarios/driver-debugging.md)
- Reference: [launch](docs/reference/launch.md) · [attach](docs/reference/attach.md) ·
  [features](docs/reference/features.md)

Contributors: see [CONTRIBUTING.md](CONTRIBUTING.md) and [CLAUDE.md](CLAUDE.md) (architecture and
conventions).

## Build

The Ninja build needs the MSVC toolchain. The npm scripts enter the Visual Studio developer
shell automatically, so they run from any shell:

```powershell
npm run configure   # configure (Ninja, vcpkg from VCPKG_ROOT)
npm run build       # build Debug
```

Equivalently, from an "x64 Native Tools for VS" prompt (so `cl`/`link` are on PATH):

```powershell
cmake --preset windows-x64
cmake --build --preset windows-debug
```

vcpkg comes from `$env:VCPKG_ROOT`. The developer shell sets that to the copy bundled with
Visual Studio, so no separate vcpkg install is needed; set it yourself to use a different one.

## Test

```powershell
npm test            # ctest --preset windows-debug
npm run test:replay # just the recorded-session replay tests
npm run test:release # configure the release tree first (npm run configure:release), then ctest
```

The test debuggees under `test-targets/testapp` always build unoptimized with full
debug info (even in Release), so their line tables and locals stay stable for the
recorded sessions and source-line assertions. Tests resolve the adapter and
debuggees relative to the test binary, so both the Debug and Release trees run
end-to-end.

The suite is unit/integration GoogleTests plus out-of-process replay tests that spawn
`dap-dbgeng.exe` and replay recorded sessions against the `test-targets/testapp` debuggees
(built as part of the tree). Tests that need `dbgeng.dll` or the native debuggees skip cleanly
when those are unavailable.

## Protocol

`src/protocol/` is generated from `protgen/schema/debugAdapterProtocol.json`. Regenerate with
`npm run generate` rather than hand-editing.

## Kernel test driver

`test-targets/sys` is a small WDK control-device kernel driver for manually exercising
kernel-mode attach. It is a separate Visual Studio + WDK build (it does not build under Ninja and is not part
of the automated tests): `npm run build:sys`, or configure the main tree with the VS generator and
`-DDAP_DBGENG_BUILD_KERNEL_DRIVER=ON`. See `test-targets/sys/README.md`.

## Format

```powershell
pwsh scripts/Format.ps1            # changed files only (git-aware)
pwsh scripts/Format.ps1 -Path src
```

## Contributing

Bugs and feature requests go through the
[issue tracker](https://github.com/svnscha/dap-dbgeng/issues/new/choose) (templates guide what to
include). For code changes, run `npm run check` and see [CONTRIBUTING.md](CONTRIBUTING.md).

## Demos

| Remote process | Kernel driver | Local program |
| --- | --- | --- |
| [![Debug a remote process](https://img.youtube.com/vi/_YUhyKK2PlI/hqdefault.jpg)](https://www.youtube.com/watch?v=_YUhyKK2PlI) | [![Debug a Windows driver](https://img.youtube.com/vi/m1E5AsglKiQ/hqdefault.jpg)](https://www.youtube.com/watch?v=m1E5AsglKiQ) | [![Debug a local program](https://img.youtube.com/vi/FLM9hPu4xfQ/hqdefault.jpg)](https://www.youtube.com/watch?v=FLM9hPu4xfQ) |

## Links

- [Install from the VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=svnscha.vscode-dap-dbgeng)
- Deep dive: [Native Windows kernel and remote debugging in VS Code](https://svnscha.de/posts/native-windows-kernel-remote-debugging-vscode/)
- Discussion: [r/vscode on Reddit](https://www.reddit.com/r/vscode/comments/1tzsk5b/new_extension_native_windows_debugging_dbgeng/)
- Announcement: [on LinkedIn](https://www.linkedin.com/posts/svnscha_native-windows-debugging-in-vs-code-cc-share-7469537648555159552-Fubb/)

## License

[MIT](LICENSE).
