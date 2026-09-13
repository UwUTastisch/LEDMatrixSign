# API specification

`ledmatrixsign-api-2.0.yaml` documents the 2.0 REST API (OpenAPI 3.0.3),
checked line by line against `src/api/api_v2.h`, `src/anim/composition.h`,
`src/anim/framefactory.h`, `src/gfx/` and `src/storage.h`.

Render it with any OpenAPI viewer, e.g. `npx @redocly/cli preview-docs
openapi/ledmatrixsign-api-2.0.yaml`.

`API-AUDIT.md` lists what the previous version of the document got wrong, what
it left out, and five firmware issues the audit turned up — two of which are
worth fixing (an animation with no frames and infinite cycles hangs the render
loop; `animname`/`filename` reach the filesystem unvalidated).