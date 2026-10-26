# LoreSourceControl

Unreal Engine 5 revision control provider for Lore, Epic's version control system.
It works like the Perforce provider: check out, submit, sync, history and so on, from inside the editor.

> Experimental. Linux only for now.

> **Disclaimer:** This is an independent community project. It is not made, endorsed or supported by Epic Games, and it is not affiliated with Unreal Engine or Lore. Unreal Engine and Lore are trademarks or registered trademarks of Epic Games, Inc.

## Requirements

- Linux
- Unreal Engine 5.8
- Lore CLI 0.8.x (`lore`) and a running Lore server
- `diff3` (from `diffutils`, usually already installed) for merging text files on Sync

## Install

1. Copy (or symlink) this folder into your project's `Plugins/` folder:
   ```
   MyProject/Plugins/LoreSourceControl
   ```
2. Build the project (or open it and let the editor build the plugin).
3. Open the editor, enable **LoreSourceControl** in **Edit → Plugins** if it isn't already, and restart.

## Setup

1. Click **Revision Control** (bottom-right of the editor) → **Connect to Revision Control**.
2. Choose **Lore** as the provider.
3. Fill in:
   - **Server URL**, for example `lore://192.168.1.10:41337`
   - **Repository**, the name on the server (only needed for a new repository)
   - **Identity**, for example `you@studio.com`
   - **Lore executable**, leave empty to auto-detect
4. If the project already has a `.lore` folder, click **Check Connection**.
   If not, click **Set Up Repository** to create the repository on the server and upload the project.
5. Click **Accept Settings**.

## Features

- Status icons in the Content Browser (checked out, modified, added, out of date)
- **Check Out** = exclusive lock on the server
- **Submit Content** (assets and `Config/*.ini` files)
- **Revert**, **Mark for Add**, **Delete**
- **Sync**:
  - Text files you changed locally are merged automatically with `diff3`.
  - For assets changed on both sides, you choose **Keep mine** or **Take theirs**. Your version is backed up to `Saved/LoreSourceControl/SyncBackups/`.
- If someone else submitted first, Submit merges with their changes and uploads again
- **History** and **Diff Against Depot** (Blueprints and other assets)
- **Restore Revision**: right-click an asset → **Lore → Restore Revision...**
- **Rename/move** tracking, so history follows the asset
- **Cancel** button for long Sync, Submit and status operations
- Background refresh every 60 seconds

## Known limitations

- No changelists, shelving or branches yet.
- Without user accounts on the Lore server, lock owners show as "another user".
- Sync always syncs the whole branch, not single files.
- Map files have no visual diff. Unreal falls back to a text diff, which needs an external diff tool (for example `meld`).
- Windows and macOS are not supported yet.

## Logs

Start the editor with verbose plugin logging:

```
UnrealEditor MyProject.uproject -LogCmds="LogLoreSC Verbose"
```

Every `lore` command the plugin runs appears in the log under `LogLoreSC`.