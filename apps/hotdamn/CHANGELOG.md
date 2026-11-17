# Hot DAMN NFO Viewer — changelog

Each release's notes on GitHub are taken from its section here, so add one
before tagging `hotdamn-vX.Y.Z` (the release build fails without it).

## 0.2.0

- **Automatic updates.** Hot DAMN checks GitHub for a new version once a day, and on demand under **Help → Check for Updates**. When one is out you can install it with one click (Update Now), open the release page, skip that version, or be reminded later.
- The installer only asks about making Hot DAMN the default `.nfo` viewer on a first install, not on every update.

Versions before 0.2.0 can't update themselves: install this one by hand once, and later versions arrive automatically.

## 0.1.2

- **Damaged NFOs recovered.** An NFO that some tool once re-saved in the wrong encoding has lost most of its art for good, and used to show as replacement characters mixed with stray Arabic letters. Hot DAMN now puts the art that survived back in its original place and shows each lost character as a grey cell. The status bar says how many characters the file lost.

## 0.1.1

First release of Hot DAMN NFO Viewer: MediaCurator's scene NFO viewer as a standalone app.

- **Art rendered as drawn.** Block, shade and line-drawing characters are painted as exact cells rather than font glyphs: no seams between rows or columns at any zoom, dotted shades like the DOS font, and cells in the 5:12 shape of DOS text mode.
- **Encoding detection.** CP437, UTF-8 and Latin-1 are told apart automatically, and DOS symbol characters (bullets, arrows, card suits) show as symbols instead of empty boxes.
- **Black on white whatever your Windows theme**, the way the art was drawn; night colours are one click away.
- **Links and images.** BBCode and links are rendered, and linked screenshots are shown inline.
- **Conveniences.** Drag and drop, zoom, a choice of fonts, copy on select, always on top, reuse of the open window, and a status bar with the file's date, size and columns × rows.
- **File association.** The installer registers Hot DAMN for `.nfo` and `.diz` and offers to open Windows' Default apps settings, where you confirm it as the default (Windows doesn't let an installer do that for you).
