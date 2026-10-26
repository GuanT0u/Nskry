# Optional local image recognition v1

Contract: `sdk/nskry_image_recognition.h`. This is a narrow, optional API 2
extension, not a generic dependency resolver or a change to API 2 layouts or
reserved fields. `requires_plugins` remains unsupported. Existing plugins work
unchanged. An older Core ignores this metadata; clients must operate without a
connected service and must still allow a persisted opt-in to be switched off.

## Discovery and use

Providers declare `extensions.image_recognition: 1` and export
`nskry_plugin_recognize_image_v1`. Clients declare
`extensions.image_recognition_client: 1` and export
`nskry_plugin_connect_image_recognition_v1`. The host connects a copied service
table before init; connection must not start work. A declared but missing export
fails loading cleanly. Metadata discovery checks enabled/compatible manifests
only: opening settings or checking availability must not load a DLL/model.

Clients explicitly name a provider ID; there is no automatic provider selection
or fallback. Translation currently offers an OFF-by-default checkbox for
`nskry-ocr`. It is optional, not an installation prerequisite. Missing, disabled,
old or busy providers are explained; no second engine is silently run.

## Ownership and threads

All service functions (`available`, `begin`, `poll`, `cancel`, `release`) are
host-UI-thread-only. Wrong-thread calls are rejected before Registry access.
`begin` borrows the caller bitmap only synchronously and copies it before
returning a nonzero opaque integer ticket. A successful begin returns PENDING,
not completed recognition. The host has one request slot, no queue. Calls are
limited to positive dimensions <=4096 per side and <=4 megapixels.

The host owns a request thread, bitmap, cancel event and bounded text/error
buffers. It initializes MTA and calls the provider off the UI thread; providers
must not touch Registry, UI or shared immediate D3D contexts there. Input bitmap,
cancel handle and output buffers are borrowed until this call returns; provider
must copy rather than adopt/delete the bitmap. No provider-allocated buffers,
callback into a client DLL, or borrowed result pointers cross this interface.

`poll` copies output on the UI thread and distinguishes PENDING, SUCCESS, FAILED,
CANCELED, UNAVAILABLE and INVALID. BUSY applies to begin. `NskryImageTextV1`
requires text capacity >=16001 and error capacity >=256, both including NUL.
SUCCESS requires nonempty, valid UTF-16 text <=16000 units without embedded NUL.
Errors must not include source text/credentials. Invalid status/length/text is a
failure, not successful partial recognition. Cancel takes precedence over a
finished result; no old text may be published as a new screenshot's result.

`cancel` only signals. Poll only while the task is active, using an existing UI
task timer; no idle timers or permanent worker threads. `release` signals,
joins, frees resources and invalidates the ticket; repeat/stale release is a
no-op. Release a terminal request before starting another. A client must release
on close, owner destruction, timer/thread creation failure and shutdown. Release
of pending work may briefly wait for cooperative cancellation; it must not be
used as a regular busy polling operation.

Core joins the request BEFORE unloading any DLL at shutdown, independent of
provider/client map order. Provider unload also cancels/joins first. Disabling
blocks new requests, not an already accepted request. Updating/uninstalling keeps
the existing restart-to-apply behavior. Tickets do not survive reinitialization
or grant access to another plugin's private model directory.

## Provider requirements and resource limits

Provider cancellation must be checked promptly; calls should finish within 30s,
including shutdown of owned processes. There is no safe host thread-kill for an
uncooperative native DLL: native plugins are trusted code, not a sandbox. Host
cannot enforce a hard timeout inside arbitrary third-party native code.

The official OCR provider reuses its selected Accurate/Fast pipeline without
opening its result/progress UI or sharing cancellation generations with OCR UI.
Accurate is one owned native process, <=4 inference threads, 20s timeout, 1GiB
process memory cap; the process exits before returning. Fast retains conditional
Windows OCR arbitration, at most 3 passes, with one request deadline and status
polling only during an operation. It does not install a Completed callback into
the plugin DLL. No encoding/model load in init. Recognition finishes/releases
before translation starts; online translation still asks upload consent.

Current Translation and OCR Accurate use the same PP-OCRv4 model. This integration
permits the OCR plugin's selected mode and future independent upgrades; it is NOT
a claim of inherently better recognition merely because OCR was installed.
