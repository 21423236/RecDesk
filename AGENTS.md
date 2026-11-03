# AGENTS.md

Repository guidance for coding agents working in this project.

## Overview
- Native C++ migration: `native/` is a separate Windows C++20 / Win32 / WASAPI + FFmpeg project. See `native/README.md` for architecture and recovery guarantees. Keep the legacy Electron app intact unless explicitly asked otherwise.
- App: RecDesk - Electron desktop screen recorder.
- Stack: Electron + electron-vite + React 19 + TypeScript.
- Package manager: npm (`package-lock.json`).
- Module mode: ESM (`"type": "module"`).
- TypeScript: strict mode enabled.
- Build outputs: `out/main`, `out/preload`, `out/renderer`.

## Project Layout
- `src/main/index.ts`: Electron main process, window creation, IPC, FFmpeg conversion.
- `src/preload/index.ts`: preload bridge.
- `src/preload/global.d.ts`: global typing for `window.recorderApi`.
- `src/renderer/index.html`: renderer entry HTML.
- `src/renderer/src/main.tsx`: React bootstrap.
- `src/renderer/src/App.tsx`: main UI and recording flow.
- `src/renderer/src/styles.css`: renderer styling.
- `electron.vite.config.ts`: build config.

## Commands
Run all commands from the repo root.

### Install
```bash
npm install
```

### Development
```bash
npm run dev
```
Starts the Electron app in development mode.

### Build
```bash
npm run build
```
Builds main, preload, and renderer targets.

### Preview
```bash
npm run preview
```
Runs the built app locally.

### Typecheck
```bash
npm run typecheck
```
Runs `tsc --noEmit`.

### Lint / Format
- No lint script exists.
- No formatter script exists.
- No ESLint config exists in the repo root.
- No Prettier config exists in the repo root.

Do not invent `npm run lint` or `npm run format` unless you add and document them.

### Tests
- No Electron test script exists in `package.json`; Electron still has no test runner.
- Native C++ tests are in `native/tests/` and registered through CTest.

### Native C++ Build / Tests
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native\build.ps1
ctest --test-dir native/build -C Release --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_core$' --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_audio$' --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_export$' --output-on-failure
ctest --test-dir native/build -C Release -R '^recdesk_ui$' --output-on-failure
```
- Requirements: Visual Studio C++ desktop workload, Windows SDK, CMake, FFmpeg with gdigrab/libx264/aac.
- Native build output: `native/release/RecDesk.exe` with `ffmpeg.exe` beside it.
- Optional npm entry points: `npm run build:native` builds/tests/packages; `npm run test:native` runs native tests after a build. Original npm dev/build commands still target Electron.
- Core / synthetic export tests never capture the screen or microphone. `recdesk_smoke.exe` is an opt-in live desktop/device test; only run it with an explicit output path and non-sensitive screen content.
- Native UI tests briefly open settings without recording. Default settings must use WDA_NONE, not WDA_EXCLUDEFROMCAPTURE; otherwise remote/screen-sharing users may see no parameters. Capture exclusion is opt-in during recording with `--exclude-controls`.
- Native code uses C++20 and Win32 Unicode APIs; do not introduce Qt or Electron dependencies to it.
- Preserve bounded audio buffers, background progress draining, graceful stop deadlines, atomic final export, and source retention on failure/cancellation.
- Audio processing lives in `native/src/audio_processing.*`. Preserve interpolation phase across packets and source-specific overwrite/deduplication; never independently ceil packet ends and add repeated samples from the same source. Synthetic audio tests do not open audio devices.
- Single audio case: `native\build\Release\recdesk_audio_tests.exe --case boundaries` (also timeline/resampling/mixing/discontinuities/clocks/offsets). This exercises the same processing code as WASAPI capture, without recording the user.
- Do not name local headers `process.h`: it shadows MSVC's CRT header used by `<thread>`.

### Running a Single Test
Electron has no supported single-test command. For native C++, use the CTest `-R` commands above.

If you add a test framework, update this file with:
1. the main test command,
2. the single-file test command,
3. the single-test-case command,
4. any Electron-specific caveats.

## Repo Rules
The repository currently does **not** contain:
- `.cursor/rules/`
- `.cursorrules`
- `.github/copilot-instructions.md`

This file is the primary agent guidance document for the repo.

## Code Style

### General
- Match the existing MVP style.
- Prefer small, direct changes over broad refactors.
- Keep main, preload, and renderer responsibilities clearly separated.
- Preserve strict typing.
- Keep user-facing copy aligned with the current Chinese UI unless the task explicitly changes language.

### Imports
- Use ESM imports everywhere.
- Use the `node:` prefix for Node built-ins.
- Prefer named imports when available.
- Use relative imports for local renderer files.
- Keep imports grouped logically: external first, local after.

### Formatting
- Follow the existing two-space indentation.
- Prefer single quotes in TypeScript and TSX.
- Use semicolon-free TypeScript, matching the current files.
- Keep multiline JSX and function calls readable rather than aggressively condensed.

### Types
- Keep TypeScript strict.
- Add explicit parameter and return types where the codebase already does so.
- Use `type` for unions and lightweight aliases.
- Use `interface` for structured object shapes when clearer.
- Use `Record<K, V>` for keyed maps when helpful.
- Update `src/preload/global.d.ts` when changing preload-exposed APIs.
- Do not introduce `any`, `@ts-ignore`, or similar type suppression.

### Naming
- camelCase for variables and functions.
- PascalCase for components, interfaces, and type names.
- kebab-case for CSS classes.
- Prefer descriptive verb-led helper names.
- Prefix UI handlers with `handle`.
- Keep IPC channels in `domain:action` format, e.g. `recording:save`.

### React / Renderer
- Use function components and hooks.
- Follow the existing `useState`, `useRef`, `useMemo`, `useCallback`, and `useEffect` patterns.
- Use early returns for invalid DOM or media states.
- Clean up streams, animation frames, and related browser resources.
- Keep UI state transitions explicit through the existing `status` pattern.

### Electron / IPC
- Keep privileged and system-level work in the main process.
- Expose only minimal typed APIs from preload with `contextBridge.exposeInMainWorld`.
- Use `ipcMain.handle` and `ipcRenderer.invoke` for request-response flows.
- Update preload runtime code and typings together.
- Preserve `contextIsolation: true` assumptions.

### Error Handling
- Throw descriptive `Error` objects for exceptional main-process failures.
- In async UI code, use `try` / `catch` / `finally` around operations that allocate resources.
- Narrow caught errors with `error instanceof Error` before reading `.message`.
- Provide user-facing fallback messages when needed.
- Always clean up temp files, streams, and animation handles even on failure.

### CSS / UI
- Keep styles in `src/renderer/src/styles.css` unless a task requires a structural change.
- Follow the existing dark visual language and semantic class names.
- Keep transitions subtle; the current stylesheet commonly uses `160ms` timing.

### File Organization
- Keep Electron main-process logic out of renderer files.
- Keep preload small and focused on the bridge.
- Prefer extracting helpers or components over growing `App.tsx` further.
- Keep renderer-only styles in `styles.css` unless a deliberate new pattern is introduced.

## Working Safely
- Decide first whether the change belongs in main, preload, renderer, or shared typing.
- When changing an IPC contract, update all affected layers together.
- Run `npm run typecheck` after TypeScript changes.
- Run `npm run build` when changing bundling, entry points, or preload wiring.
- Do not claim Electron tests passed; there are no Electron tests. For native changes, run CTest and report any skipped tests honestly.
- If you add linting, formatting, or testing, update `package.json` scripts and this file in the same change.

## Known Gaps
- No lint setup.
- No formatter setup.
- No automated Electron tests; native C++ core, synthetic audio, synthetic export and UI tests use CTest.
- `App.tsx` is already large, so feature work should avoid making it harder to navigate.

If you introduce any of those systems, update this document immediately so the next agent has accurate instructions.
