# Translate a query into media search fields

In aMule or amulegui, open **Extended Parameters** in the Search panel and choose
**Translate query...**. Enter keywords with explicit media labels, then choose
**Preview**. For example:

```text
jazz artist:"Miles Davis" album:"Kind of Blue" duration:>=5m bitrate:>=192
```

The preview shows `jazz` as the keywords, `Miles Davis` as the artist,
`Kind of Blue` as the album, 300 seconds as the minimum duration and 192 kbps as
the minimum bitrate. Edit any preview field, then choose **Use preview** to copy
it to the search form. Press **Search** separately when ready.

Applying a preview replaces the keywords and all six media filters, including
clearing filters absent from the query. File type, extension, size, availability,
network and download category keep their existing settings. Cancel leaves the
search form unchanged. Editing the source query disables **Use preview** until
you preview it again; editing the preview itself does not retranslate it.

## Supported syntax

| Label | Value |
| --- | --- |
| `artist:` | Artist text |
| `album:` | Album text |
| `title:` | Title text |
| `codec:` | Codec text, such as `FLAC` |
| `duration:` | Minimum seconds, `h:mm:ss`, or a whole number with `s`, `m` or `h` |
| `bitrate:` | Minimum whole-number bitrate in kbps |

Labels are case-insensitive. Put values containing spaces in double quotes.
Inside a quoted filter value, use `\"` for a double quote and `\\` for a
backslash. Each label can appear only once and must have a nonempty value.

Numeric values are inclusive minimums by default: `duration:300` and
`duration:>=5m` both mean at least 300 seconds. `duration:>5m` means at least 301
seconds, and `bitrate:>192` means at least 193 kbps. Duration is limited to 24
hours and bitrate to 1000000 kbps. Maximums, equality operators, negative values
and fractional values are not supported.

Unlabelled words remain keywords; the translator does not guess artists or
albums from ordinary language. Quoted keyword text and existing `OR` / `NOT`
operators are preserved, while whitespace between keyword tokens is normalized.
Media filters apply to the whole keyword expression. Place them outside
parentheses, without joining a filter to `AND`, `OR` or `NOT`. For example,
`(jazz OR blues) artist:Queen` is supported, while `jazz OR artist:Queen` is
rejected because it would require an OR between a keyword and a media filter.
Keywords are required;
filters alone cannot start a search.

Unknown labels, duplicate filters, unclosed quotes and invalid numbers produce
an error without replacing the last preview. To search literally for a token
containing a colon, quote the whole token, for example `"genre:jazz"`. URL tokens
such as `https://example.test/music` remain keywords.

## Network support

This helper fills the existing advanced search fields; it does not change the
search protocol or add automatic natural-language interpretation. Minimum
bitrate is effective on Kad; eD2k servers may ignore media filters. Kad still
requires you to stop an active search before changing filters for the same
keyword. In amulegui, the translator is disabled when the daemon does not support
media filters.

## Offline checks

`SearchQueryTranslatorTest` runs without a display or network and covers labels,
quoting, Unicode, keywords, units, numeric bounds and rejected input.
`SearchQueryDialogTest` checks editable previews, validation, invalidation after
query edits and cancellation using native widgets. It needs a graphical display
and reports a CTest skip when none is available. Neither test connects to peers.
