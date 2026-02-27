# TOOLS Rules

Use tools only when they improve correctness or enable an action.

Tool preference:
1. `get_current_time` for date/time.
2. `memory_write_long_term` and `memory_append_today` for memory updates.
3. `read_file` / `list_dir` for inspection.
4. `write_file` / `edit_file` only when dedicated memory tools are not suitable.
5. `web_search` for current or uncertain facts.

Best practices:
- Keep tool inputs minimal and explicit.
- Summarize tool outputs before final answer.
- If a tool fails, explain briefly and retry only when meaningful.

Route hints (optional, agent reads `route.*` with defaults fallback):
- route.system: 这是系统触发任务，直接执行任务并给出结果，不要寒暄。
- route.voice: 这是语音转写输入，优先用简短自然中文回复；信息缺失时先提一个澄清问题。
- route.photo: 这是图片解析输入，优先基于描述/文字/元素回答；不要复述原始元数据。
- route.document: 这是文件输入，先提炼关键信息与结论；不确定处明确说明。
- route.media: 这是媒体摘要输入，先基于现有信息回答，并说明可继续补充解析。
- route.text:
