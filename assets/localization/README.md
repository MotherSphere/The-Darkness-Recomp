# Translating the added PC menus

`subtitle_menu.json` contains the Subtitle Size label and its help footer for
English, German, French, Spanish, and Italian. These rows use the effective game
language selected at startup. The rest of each subtitle menu retains the original
dump's translated categories. Other added PC-menu labels retain their current
English/Russian coverage.

`native_menu_ru.json` contains the Russian text for the added in-game settings,
keyboard/mouse bindings, and exit confirmation, including Subtitle Size, live
values and feedback.

Save the file as UTF-8. Edit each `text` value, retaining its uppercase English key
and `max_glyphs` limit. For example:

```json
"EXIT GAME": {"text": "ВЫЙТИ ИЗ ИГРЫ", "max_glyphs": 20}
```

The original menu grid has fixed widths. Use compact uppercase translations within
those limits. Physical key names such as W, Ctrl and LMB remain recognizable.
The subtitle catalog requires all five language sections and both uppercase
English keys. Its fixed widths are 16 glyphs for the size label and 40 for the
help footer. The generator validates uppercase text, CP1252 encoding and these
limits without truncating or changing the source file. The stock Western font
supports the ASCII and Latin-1 accented bytes used here; CP1252 bytes 0x80–0x9F
are rejected because their punctuation and ligature glyphs are absent. Use straight
quotes instead of curly quotes, and avoid the euro symbol. Compact abbreviations
keep the translations within the original grid.

The Russian generator checks Cyrillic encoding and layout limits, then converts
the text to CP1251 bytes for the Russian dump's original font. Paste translations
into the UTF-8 catalogs rather than generated XCR files or C++ byte strings.

From a source checkout with your own game files, run:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

This regenerates the runtime subtitle text dictionary, the Russian loose menu
(`CubeWnd.pc.ru.xcr`), and the compiled Russian menu dictionary from the same
catalogs. Rebuild after editing either catalog so static labels and runtime text
stay consistent. Generated menus are staged beside the executable and included
in Windows releases and Linux setup.

Russian menus activate when the runtime recognizes an approved Russian executable
and uses the game's English content slot. Choose **English**, or **System default**
on Russian Windows. The original dump supplies the other translated game strings,
fonts and audio; keep its executable and matching assets together. Language changes
take effect after restarting.
