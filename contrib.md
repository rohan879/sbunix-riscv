# Development

- All development should take place on seperate branches
- Changes and updates should be tested (locally) prior to being merged into main

# Commit Message Prefixes

| Prefix | Meaning |
|--------|---------|
| **BF** | Bug fix |
| **RF** | Refactoring |
| **ENH** | Enhancement/new feature |
| **BW** | Addresses backward-compatibility |
| **OPT** | Optimization (performance improvements) |
| **BK** | Breaks something and/or tests fail |
| **DOC** | For all kinds of documentation related commits |
| **TEST** | For adding or changing tests |
| **MNT** | For administrative/maintenance changes |
| **CI** | For continuous-integration changes |
| **STY** | Commits that do not affect the meaning (white-space, formatting, missing semi-colons, etc), but change the style |
| **BLD** | Commits that affect build components such as build tool, ci pipeline, dependencies, project version etc. |
| **OPS** | Commits that affect operational components like infrastructure, deployment, backup, recovery etc. |
| **CHORE** | Miscellaneous commits (e.g. modifying .gitignore) |
| **API** | An (incompatible) API change |
| **DEV** | Development tool or utility |
| **REV** | Revert an earlier commit |
| **MERGE**| Merging branches/histories |

> **Note:** Breaking changes should be indicated with an exclamation mark before the colon (e.g., `ENH!: some breaking enhancement`).

# Commit Messages

Commit messages should:
- Start with any of the relevent prefixes shown above, e.g.: ENH: | BF: | RF: | DOC: | MNT: | TEST: | CHORE: etc.
- Capitalize the first letter after the ':'
- Be no more than 8 - 12 words max
- Be in past-tense

After the commit message one-line, a more descriptive message may be written if the changes are substantial (e.g. complex changes/control flow, large changes across multiple files, etc).