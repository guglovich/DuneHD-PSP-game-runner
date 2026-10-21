# GLM-5.3-Flash Architecture Guidelines

## Architecture & Code Integrity
- **No Stubs/Placeholders:** Never leave `TODO`, `// implement later`, or incomplete function bodies. Write full, production-ready code.
- **Architectural Coherence:** Always examine existing interfaces, types, and project conventions before modifying files.
- **Single-Pass Completeness:** Process the entire requirement scope at once. Do not split logic into uncoordinated partial edits.

## Thinking & Execution Protocol
1. **Repository Context First:** Inspect target files, imports, and references before proposing changes.
2. **Refusal to Guess:** If crucial project constraints or API signatures are ambiguous, ask for clarification instead of making assumptions.
3. **Refactoring Safety:** Ensure modifications do not break existing signatures, tests, or state management logic.

## Formatting Standards
- Output code in precise Markdown blocks specifying the language and file target.
- Keep explanatory text concise and actionable.