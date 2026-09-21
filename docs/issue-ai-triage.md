# AI issue triage

Newly opened GitHub issues can receive an automated first-pass triage response from an OpenAI model.

## What it does

The workflow `.github/workflows/issue-ai-triage.yml`:

1. starts when an issue is opened;
2. checks out the current `develop` branch as its Quidra knowledge source;
3. provides the model with the issue title/body plus the current README, language specification, architecture, diagnostics, grammar, LLM guide, numeric/bin specification, package documentation, manifest, project metadata, and development documentation;
4. classifies the issue as a bug, feature, documentation request, question, or other;
5. classifies its area as Core, Vision, DNN, or unknown;
6. decides whether material reproduction information is missing;
7. adds only workflow-controlled labels from a fixed allowlist; and
8. posts a concise reply in the issue author's language.

The generated reply is explicitly marked as automated initial triage. It does not close issues, assign maintainers, modify code, or promise fixes.

## Required repository secret

Create an Actions repository secret named `OPENAI_API_KEY` containing an OpenAI API key. The workflow intentionally does not store an API key in the repository.

The default model is `gpt-5.6-terra`. Change `OPENAI_MODEL` in the workflow if a different model is desired.

## Security boundary

Issue titles and bodies are treated as untrusted data. They are never interpolated into shell commands. The model is instructed not to follow commands embedded in issues, and model output cannot select arbitrary GitHub labels. Instead, the Python triage script validates the model's JSON classification and maps it to a fixed label allowlist.

The workflow has only:

- `contents: read`
- `issues: write`

It does not receive repository-content write permission.

## Managed labels

The workflow may create a missing managed label, but it does not overwrite an existing label definition. It may apply:

- `ai-triaged`
- `needs-info`
- `bug`
- `enhancement`
- `documentation`
- `question`
- `area: core`
- `area: vision`
- `area: dnn`

## Manual triage

The workflow also supports `workflow_dispatch` with an existing issue number. This is useful for testing or rerunning triage after setup.

## Activation

GitHub's `issues` event runs a workflow only when that workflow file exists on the repository's default branch. Quidra's default branch is `main`, while normal development happens on `develop`. Therefore the implementation is developed and tested on `develop`, but automatic issue-open handling becomes active after the workflow reaches `main` through the normal release/integration process.
