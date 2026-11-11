# Notice: origin of this page

This folder is a trimmed copy of the Bambu Studio web page that implements the Filament Manager.

- Origin: Bambu Studio, `src/slic3r/GUI/DeviceWeb/device_page`
- Tag `v02.09.01.70`, commit `e460a9382`
- License: GNU Affero General Public License v3 (GNU AGPL v3)
- Copyright: Bambu Lab
- The tab icon `resources/images/tab_filament_active.svg` is from the same Bambu Studio tag, same licence.

The rest of this repository is AGPL v3 as well. The built bundle in `resources/web/filament_manager/` is generated from this folder by `scripts/build_filament_manager_page.sh`.

`README.md` in this folder is the unmodified upstream README (it still talks about pnpm and the removed device pages; this folder builds with npm and `package-lock.json`).

## What was left out of the copy

- `node_modules`, `dist`, `pnpm-lock.yaml`, `README_zh.md`, `test-results`.
- `public/` (unused images and a favicon the Filament Manager does not reference).
- `playwright.config.ts`, `tests/e2e/`, and the unit tests that exercise removed code (`tests/amsHotendContentSize.test.ts`, `cardTextSource.test.ts`, `hotendUsedTimeSource.test.mjs`, `printQueue.test.ts`). The three tests of Filament Manager code stay in `tests/`.
- `i18next-parser.config.js` (the `i18n` script). The `locales/` files are kept unchanged, including keys for removed screens.

## What was removed from `src/` (route `/filament_manager` and everything it imports is kept)

- `routes/about.tsx`, `routes/app.tsx`, `routes/device_page/` (12 routes).
- `features/device-page/` (AMS control, AMS/hotend, AMS mapping, auto-refill, connected slots, print queue, with their assets).
- `features/printer/` (AMS, control, extruder, live view, machine, motion, task demo components).
- `features/utils/`, `components/`, `utils/` (except `WindowResizeListener.tsx`), `hooks/` (except `Bridge.tsx`: `CenterVector`, `Raf`, `useDeviceWebTheme`, `useFitHostWidth` removed).
- `features/filament-manager/StatsView.tsx` and `SpoolSvg.tsx` (nothing imports them).
- `dev/mockBridge.ts` stays, with its device-page mock branches and imports removed.
- `routes/__root.tsx` and `routes/index.tsx` rewritten: no navigation bar; `/` and any unknown hash redirect to `/filament_manager`.
- `routeTree.gen.ts` is hand-maintained (two routes). The TanStack route-generation Vite plugin is no longer used.
- `package.json`: removed `@radix-ui/*` (4), `radix-ui`, `@tanstack/router-plugin`, `@playwright/test`, `i18next-parser`, the `e2e`/`i18n` scripts, the `packageManager` and `pnpm` fields; package renamed `filament_manager_page`. `package-lock.json` was pruned by one `npm install`.

## Patches to upstream code

1. Theme (`index.html`). The inline script no longer looks for "(light)"/"(dark)" in the user-agent string. It uses the `theme` query parameter (`?theme=dark` or `?theme=light`) and, when absent, `prefers-color-scheme`. Needed with it: `src/store/FilamentSlice.tsx` `detectTheme()` (the store's initial theme, which `FilamentManagerPage` writes to `<html data-theme>` on mount) now reads `document.documentElement.dataset.theme` instead of the user agent; otherwise it would overwrite the chosen theme with `dark`. The `init` response still sets the theme through the store. The script also sets `document.documentElement.style.colorScheme` to the chosen theme, so the browser draws dark scroll bars in dark mode.
2. No sourcemaps (`vite.config.ts`: `sourcemap: false`).
3. Title (`index.html`): `<title>Filament Manager</title>`.
4. Needed to build the trimmed app:
   - `vite.config.ts`: removed the `TanStackRouterVite` plugin and its import (see `routeTree.gen.ts`).
   - `index.html`: removed `<link rel="icon" href="/vite.svg">` (the file was in the removed `public/`).
   - `src/routes/__root.tsx`, `src/routes/index.tsx`, `src/routeTree.gen.ts`: as described above.
   - `src/dev/mockBridge.ts`: removed the device-page branches.
5. "Push Local to Cloud" toolbar button (`src/features/filament-manager/FilamentManagerPage.tsx`): the button, its `pushingAll` state and its click handler are removed, and `pushAllNow` is no longer taken from the bridge hook there. The button pushes printer-linked spool data, which this build does not support yet; `pushAllNow` stays in `useFilamentManagerBridge.ts`.
6. Custom filaments read from an AMS slot (`src/features/filament-manager/buildSpoolFromTray.ts`, `AddEditDialog.tsx`). The tray-to-preset lookup still skips `is_user` items, except when the slot's filament id is a custom filament id (`P` followed by hex digits, new helper `isCustomFilamentId`), so a slot loaded with a custom filament is imported with that filament's brand, type and name instead of an empty brand; ids of system filaments keep upstream's rule.

## Updating to a newer Bambu Studio tag

1. Export the folder: `git -C <BambuStudio> archive <tag> src/slic3r/GUI/DeviceWeb/device_page | tar -x -C <scratch>`; do not check the tag out.
2. Replace the contents of this folder with the export (leave out `node_modules`, `dist`, `pnpm-lock.yaml`, `README_zh.md`), keeping `package-lock.json`, `.gitignore`, `NOTICE.md`, `BRIDGE.md`.
3. Re-apply the removals and patches listed above (a reachability walk from `src/main.tsx` over relative imports finds the dead files), then run `npm install` once to prune the lock file.
4. Run `scripts/build_filament_manager_page.sh`, diff `BRIDGE.md` against the new `useFilamentManagerBridge.ts` / `hooks/Bridge.tsx` / `AddEditDialog.tsx`, update the tag and commit in this file.
