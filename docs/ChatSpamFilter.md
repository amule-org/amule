# Chat protection in amule and amuled

The advanced spam filter now protects incoming chat in both builds. Configure it
under **Preferences → Filters** in amule, amulegui, or the WebUI. REST clients can
PATCH `/api/v1/preferences` with:

```json
{"message_filter":{"advanced_spam_filter":true,"use_captchas":true}}
```

With the advanced filter enabled, unsolicited links and repeated unanswered
messages from non-friends are blocked. With CAPTCHA enabled as well, a peer
who writes first must solve a challenge before the original message reaches
the transcript. Friends and peers we have already written to bypass this
challenge. A correct answer submits the original message to the spam filter;
unsolicited links and repeated unanswered messages remain blocked.
Answers ignore letter case and accept `S/5`, `Z/2`, `G/6`, and `B/8` as equivalent.
Incorrect answers allow at most three challenges. Peers without CAPTCHA support
receive a single explanation and cannot start a conversation.

Pending messages stay hidden. Creating transcript entries for every blocked
sender would allow spam to fill the session list. The existing CAPTCHA-answer
interface continues to display challenges that other peers send to us.

Both settings default to **on** in amule and amuled. Saved values of
`/eMule/AdvancedSpamFilter` and `/eMule/MessageUseCaptchas` are preserved.
Disabling the advanced filter also bypasses CAPTCHA without changing the saved
CAPTCHA sub-option.

The REST field `message_filter.advanced_spam_filter_supported` is read-only.
It is false when connected to an older daemon; writes to the two new options
then return HTTP 409. New desktop clients disable those controls for older
daemons. Older clients saving the message-filter preferences leave both new
settings unchanged.
