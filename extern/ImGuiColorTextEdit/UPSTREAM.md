Source: https://github.com/BalazsJako/ImGuiColorTextEdit
Commit: ca2f9f1462e3b60e56351bc466acda448c5ea50d
License: MIT (see LICENSE).
Local patches are documented in this file.

Local patches: ReplaceSelection with text undo; exact GetText final-newline preservation; caret byte indices and hover coordinates; remove unused lineSize to satisfy /WX.

Semantic highlighting: extra symbol palette slots and a non-destructive compiler span overlay. Spans use zero-based UTF-8 byte columns, are validated against each source line, and are cleared on source edits. Rendering applies the overlay without modifying lexical tokens or text undo history.
