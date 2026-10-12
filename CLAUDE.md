# minecraftCplusplus

## Agents and subagents

- Code is written by the main agent itself. Never use a Haiku or Sonnet subagent to write or edit code.
- Haiku subagents are for exploration and research only: finding code, reading the decompiled reference, summarizing
  files, answering "where/how is X done" questions. Use them freely for that instead of reading large amounts of code
  in the main context.
- Keep subagents few: at most about two running at a time; queue the rest. Running many at once burns the owner's usage.
