# Optional online engines — 0.3.0

First run defaults to Offline. The engine dropdown remembers an explicit user
selection; reopening never bypasses upload confirmation. There is no automatic
fallback, retry, endpoint discovery, key acquisition or startup connection.
All engines currently expose Chinese/English directions, not universal detection.

## Configure an engine

- OpenAI-compatible: select it, Configure engine, enter the **full** HTTPS Chat
  Completions endpoint (including `/v1/chat/completions` where applicable), your
  provider's model ID and optional API key. Standard requests use
  `max_completion_tokens: 4096`. Select the legacy `max_tokens` checkbox only for
  gateways requiring it. No default model, account or paid API is selected.
- LibreTranslate: enter the full HTTPS `/translate` endpoint and optional key.
  Some self-hosted instances permit empty keys. LibreTranslate's hosted service
  is not claimed to be a free public endpoint.
- Free online · MyMemory: built-in official public `/get` endpoint. No key,
  account or contact email required. Anonymous usage currently has a
  **5000-character daily quota**; the provider manages this, not Nskry. Per task:
  2000 UTF-8 bytes, at most 16 sequential requests and 60 seconds overall.
  Each segment stays under 480 UTF-8 bytes (below the API's 500-byte cap).
  Paragraph breaks survive; distinct paragraphs are never sent in one request.
  Quota/failure stops without retries or publication of partial results.

Saving configuration does not upload text or test connectivity. To translate,
capture/hand off text or edit the source, then approve its upload notice. Source
editing clears old output; Copy result and Save text operate on completed text.

## Privacy and safety

Every online task shows the destination and asks permission. The request contains
the current source text, direction and necessary model/instructions/credentials;
never screenshot pixels or prior-task history. Cancel/timeout cannot recall text
already received by the server or undo a provider charge. Online output is plain
text, never executable HTML. No automatic fallback, continuous OCR upload,
provider polling or content/error-body logging is added.

**MyMemory is unsuitable for confidential text.** Its terms describe long-term
storage of submitted segments and sharing with external partners. Text is sent
via GET query parameters, which can be retained in server/terminating-intermediary
logs. Only translation retrieval is used, not contribution (`/set`), account or
key-generation APIs. Provider terms, quotas, availability and service-resale rules
govern that third-party service; they are not additional Nskry source-code license
restrictions. No unlimited/free-service availability guarantee is made.

Credentials are masked in settings. Each provider's complete versioned config is
current-user DPAPI protected in HKCU under `Software/Nskry/Plugins/nskry-translate`
(`OpenAIConfigV1` or `LibreConfigV1`). Forget removes only that provider's config.
Unknown fields survive a save; corrupt/unsupported schemas are not silently
replaced. Existing ModelRoot remains compatible. Explicit engine preference uses
PreferredProviderV1. DPAPI is not a sandbox against same-user malicious code;
there is no guarantee of erasing every allocator/OS copy of secrets.

HTTPS is mandatory, including self-hosted endpoints. URL credentials, configured
URL query parameters, redirects, cookies and implicit Windows credentials are
blocked. Standard certificate validation stays enabled. No hidden HTTP loopback
exception exists. Tasks own async WinHTTP sessions/requests and a worker, then
release them; UI/registry stay on the UI thread. Completion callbacks only touch
task-owned events/atomic state. A handle-close fence protects request storage,
and WinHTTP's session unload event waits until the final callback has actually
returned, protecting plugin shutdown/unload. Cross-session connection pooling
and automatic authentication are disabled.

Input cap: 16000 UTF-16 units (free service has the smaller byte/request caps).
Response cap: 2 MiB. No queue or automatic retry; incomplete, filtered, refused,
tool-calling or non-`stop` Chat Completions outputs are not published as complete.
Azure-specific auth/query parameters, Responses/SSE, tools, multimodal input and
arbitrary nonstandard compatibility variants are not implemented.

## Dependencies and verification

Online engines are native; they need no Python, translation model or OCR plugin.
The optional online-only package omits the complete frozen offline runtime.
Both package variants share an ID/version; choose one, not an extra prerequisite.
No package is installed automatically. The runtime license clearance gate still
applies before redistributing an offline-enabled bundle.

Ordinary tests contact only owned loopback peers. Explicit live tests used fixed
public synthetic short sentences. MyMemory succeeded in both directions and in
the actual result window. BYOK/Libre have schema, encryption and transport-lifecycle
coverage; a live success still needs the user's own configured server/key.

References checked 2026-10-25 (terms/quotas may change):

- [OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)
- [LibreTranslate usage](https://docs.libretranslate.com/guides/api_usage/)
- [MyMemory API](https://mymemory.translated.net/doc/spec.php)
- [MyMemory quotas](https://mymemory.translated.net/doc/usagelimits.php)
- [MyMemory terms/privacy](https://mymemory.translated.net/terms-and-conditions)
