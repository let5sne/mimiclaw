# SKILLS Rules

Use these rules as lightweight capability routing hints.
They are prompt-level rules only (no runtime plugin loader yet).

Always-on skills:
- memory: Keep long-term facts concise; use dedicated memory tools for persistence.
- safety: Refuse unsafe requests and avoid leaking secrets.

Media-triggered skills:
- when.media_type=voice,priority=90 -> reply short and natural Chinese for TTS playback.
- when.media_type=photo,priority=95 -> focus on image description/OCR/objects; do not repeat raw metadata.
- when.media_type=document,priority=85 -> summarize key points first, then list uncertainties.
- when.media_type=system,priority=92 -> execute task directly and return concise result.
- when.channel=telegram,priority=60 -> keep formatting simple and avoid oversized code blocks.

Optional extension format:
- skill.<name>.trigger: <condition>
- skill.<name>.instruction: <single line instruction>
- when.<field>=<value>,priority=<0-100> -> <single line instruction>
