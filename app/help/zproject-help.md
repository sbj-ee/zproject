# zproject Help

zproject is the desktop app for a zproject server (`zprojectd`) on your
tailnet. It shows your projects, keeps a read-only copy for when the server
can't be reached, and follows changes made elsewhere — another computer,
`zproject-cli`, or an agent — within about 15 seconds.

**Contents**

- [Connecting](#connecting)
- [The window](#the-window)
- [The outline](#the-outline)
- [Offline](#offline)
- [Appearance](#appearance)
- [Preferences](#preferences)
- [Updates](#updates)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Accessibility](#accessibility)
- [Privacy and security](#privacy-and-security)
- [More help](#more-help)

## Connecting

The first time it starts, zproject asks for two things:

1. **The server URL** — the `https://` address `tailscale serve` publishes,
   for example `https://server-host.example.ts.net`. If `zproject-cli` on
   this computer is already set up, its URL is filled in.
2. **A device token.** If `zproject-cli login` has stored one on this
   computer, zproject offers it by its label (`zpt_…`) and uses the same
   token, so the app and the CLI are one device. Otherwise create one on the
   server with `zproject-cli token create --kind device --name "laptop"` and
   paste it. The token is never shown on screen; it is kept in the 0600 token
   file or the Keychain, the same places `zproject-cli` uses.

To change the server or the token later, use **File → Connect to Server…**
or **Preferences → Change server or token…**.

## The window

- The **title bar** shows zproject's version and the selected project, for
  example "zproject 0.13.0 — Kitchen remodel"; a * (the dot in the close
  button on macOS) means the task editor has unsaved changes.
- The **menu bar** (File, Edit, Project, View, Help) is always in the
  window on Linux; on macOS it is at the top of the screen.
- The **toolbar** below it has the common actions: New Project, Edit
  Project Details, Refresh, Outline, Gantt, Insert Task, Task Details, Save Task,
  Find and Check for Updates. Hover a button for its name and shortcut.
  **View → Show Toolbar** hides or shows it; zproject remembers.
- **Projects** (left) lists the projects your token can see. Each has a
  folder icon with a dot in its RAG colour (green, amber, red); projects that
  aren't active are greyed and named with their status.
- The **centre** shows the selected project: its details, or its tasks in
  the outline (**View → Outline**), or its Gantt (**View → Gantt**). The
  resource views arrive in later versions.
- The **Inspector** (right) shows the selected item's details; for a task
  it is the task editor (below).
- The **status bar** shows the connection, the server's version and when the
  data was last refreshed. **Read-only** means editing is off.

Press **F5** to refresh now. zproject also refreshes every 15 seconds while
its window is active and every 60 seconds in the background.

## The outline

**View → Outline** shows the project's tasks as a tree with their WBS code,
type, status, % complete, duration, dates, deadline and priority.

- **Summary tasks** (tasks with subtasks) are bold. Their status and %
  complete are rolled up from the subtasks, so those cells show a lock and
  can't be edited; change the subtasks instead.
- **Edit in place** with a double-click or F2: the title, the priority, and
  the status and % complete of tasks without subtasks.
- **Insert Task** (Ins or Enter) adds "New task" below the selected one and
  lets you type its title. **Tab** indents a task under the one above it;
  **Shift + Tab** outdents it. **Alt + ↑ / ↓** move it among its siblings.
  **Ctrl + Tab** leaves the tree.
- **Delete Task…** (Del) deletes a task and everything under it after
  showing what goes; **Edit → Restore Deleted Task…** brings back what you
  deleted in this window, and the history keeps everything.
- **Convert to Milestone** turns a task into a zero-length milestone (shown
  with a diamond) and back.
- The **filter** above the tree shows all tasks, the ones not done, or one
  status. **Find** (Ctrl + F) searches titles or a WBS code such as 2.1;
  Enter and Shift + Enter step through matches, Esc closes it.

When a change affects more than the task itself (a summary's progress, say)
or deletes something, zproject shows what will change before writing
anything. If someone else changed the task a moment ago, nothing is
overwritten: you choose, or the outline refreshes so you can try again.

## The Gantt

**View → Gantt** (Ctrl + G) shows the project's tasks as a timeline next to
a task table. The two scroll together, and collapsing a summary in the table
hides its rows on the right. The Gantt is read-only: it shows the dates
stored on each task and changes nothing.

- **Bars**: an automatically scheduled task is a solid bar; a task with
  manual dates is an outlined bar. A summary is a bracket over its subtasks,
  a milestone is a diamond. The darker band inside a bar is its % complete.
- **Deadlines** are small flags: outlined while the finish is on time, filled
  red when the finish is after the deadline.
- **Today** is the dashed red line. **Today** (or **T**) scrolls to it.
- **Timescale**: Day, Week, Month or Quarter. Ctrl + Shift + = and
  Ctrl + Shift + − (or Ctrl + mouse wheel) zoom in and out; Shift + wheel
  scrolls sideways. Weekends are shaded at Day and Week.
- A task with no dates yet says *unscheduled* in the Start column and has no
  bar.
- **↑ / ↓** move between tasks; the inspector follows. **Enter**, or a
  double-click on a bar, opens the task editor there.
- Screen readers read each bar in words, for example "Task 1.2 Drawings, 3
  days, 9–11 Sep, 40% complete"; hovering a bar shows the same text.

## Editing a task

Select a task in the outline: the **Inspector** shows it with three tabs.
**Edit → Task Details** (Alt + Enter) jumps to its title.

- **Details**: title, type (task or milestone), mode, status, % complete,
  priority and notes. Notes are Markdown; **Preview** shows how they read.
- **Schedule**: duration, start and finish (manual tasks only; auto tasks
  get their dates from the schedule), constraint and its date, deadline, and
  the actual start and finish.
- **History**: every change to the task, newest first: when, who ("This
  computer" for yours) and what changed.

**Save** (Ctrl + S, or **Edit → Save Task**) checks the fields first: a
field with a problem is marked ⚠ with the reason and nothing is sent.
Setting % complete above 0 fills in the actual start for you. A summary
task's status, % complete and actual dates are rolled up from its subtasks,
so they are locked. When a save changes more than this task (a summary's
progress, say) zproject shows what will change and writes nothing until you
confirm. If someone else saved the task since you opened it, you choose per
field: keep yours or take theirs. **Revert** puts the task back as the
server has it. Moving to another task, another project or quitting with
unsaved changes asks first.

## Offline

When the server can't be reached, a banner says **Offline — showing data
from** the last refresh. You can keep reading everything that was loaded;
editing is off until the server answers again. zproject retries on its own
(5 seconds, then up to a minute apart); **Retry now** tries at once.

If the server refuses the device token (it was revoked or expired), the
banner offers **Sign in…** to paste a new one. A certificate problem or a
server speaking another API version is shown the same way.

## Appearance

**View → Theme** chooses Light, Dark, High contrast, or System (follow the
operating system). The accent colour is zproject blue, as in zterminal and
zwriter.

**View → Zoom In / Zoom Out / Actual Size** (Ctrl + = , Ctrl + − , Ctrl + 0;
⌘ on macOS) changes the size of all interface text from 50 % to 300 %. In
the Gantt, Ctrl + Shift + = / − and Ctrl + mouse wheel zoom the timeline
instead.

## Preferences

**File → Preferences** (Ctrl + , — **zproject → Settings** on macOS):

- **Theme** and **Text size**, as in the View menu.
- **Keep a read-only copy for offline use.** Turning it off deletes the copy
  and keeps nothing on disk.
- **Clear cache** deletes the copy and loads everything again from the
  server. It needs the server, so it is off while you are offline.
- **Check for zproject updates once a day** turns the daily update check on
  or off; **Check Now…** checks straight away.
- **Connection** shows the server, its version, and the device token by its
  label only.

## Updates

**Help → Check for Updates…** asks GitHub for the latest zproject release.
The first time zproject starts it asks whether to check once a day; you can
change that in Preferences. A daily check only speaks up when there is a new
version.

When a new version is available:

1. **Download and Verify** fetches the release's SHA256SUMS and its minisign
   signature, checks the signature against zproject's release key
   (32960BBA77B36F1F), then downloads the desktop package (the .deb on Linux,
   the .dmg on macOS) and checks its SHA-256.
2. Only when both checks pass does **Install…** appear. If anything doesn't
   match, the download is deleted and nothing is installed.
3. **Install…** on Linux runs `pkexec apt install` for the zproject desktop
   package and asks for your password; on macOS it opens the disk image for
   you to drag zproject.app to Applications. Then **Restart Now**.

**Later** closes the window; **Skip This Version** stops the daily check
offering that version. The updater never installs anything on its own and
never touches the zprojectd server package; update the server with apt on
the server machine.

## Keyboard shortcuts

| Action | Keys |
| --- | --- |
| Refresh | F5 or Ctrl + R |
| Preferences | Ctrl + , |
| Zoom in / out / actual size | Ctrl + = / Ctrl + − / Ctrl + 0 |
| Show or hide the sidebar | Ctrl + B |
| Show or hide the inspector | Ctrl + I |
| Go to the project list | Ctrl + Shift + P |
| New project / edit project details | Ctrl + N / Ctrl + E |
| Insert task (outline) | Ins or Enter |
| Indent / outdent (outline) | Tab / Shift + Tab |
| Move task up / down (outline) | Alt + ↑ / Alt + ↓ |
| Rename task (outline) | F2 |
| Edit task details / save task | Alt + Enter / Ctrl + S |
| Delete task (outline) | Del or Backspace |
| Leave the outline | Ctrl + Tab (Control, not ⌘, on macOS) |
| Find / next / previous | Ctrl + F / F3 / Shift + F3 |
| Show the Gantt | Ctrl + G |
| Gantt timescale in / out | Ctrl + Shift + = / Ctrl + Shift + − (or Ctrl + wheel) |
| Scroll the Gantt to today | T |
| Help | F1 |
| Keyboard shortcuts | Ctrl + / |
| Quit | Ctrl + Q |

On macOS, Ctrl is ⌘. **Help → Keyboard Shortcuts** lists them too.

## Accessibility

Every control can be reached with Tab and Shift + Tab and has a name for
screen readers. Menus have keyboard mnemonics (Alt + letter on Linux). The
High contrast theme uses white on black with yellow selection; every theme
keeps text at a contrast of at least 4.5 : 1.

## Privacy and security

zproject talks only to your server, over HTTPS on your tailnet, and to
GitHub over HTTPS when it checks for updates. The local
copy is a private file (readable only by you) in your cache folder. Tokens
are never written to the cache, to logs, or to the screen.

## More help

- [zproject on GitHub](https://github.com/sbj-ee/zproject)
- [API reference](https://github.com/sbj-ee/zproject/blob/main/api/openapi.yaml)
- [Report an issue](https://github.com/sbj-ee/zproject/issues)
