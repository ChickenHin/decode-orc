# Decode-Orc plugin registry

This directory hosts the **curated plugin index** — the list of third-party
stage plugins that Decode-Orc offers under *Plugin Manager → Browse Plugins…*
and `orc-cli plugins search / info / install`.

The host fetches [`index.yaml`](index.yaml) from the repository's default
branch over plain HTTPS. Because it reads the branch head, **a merged pull
request publishes immediately** — no Decode-Orc release and no registry tag are
required.

The index curates **which plugins are offered, not their versions**. Once a
plugin is accepted, the host resolves its **latest published GitHub release**
at browse and install time, and keeps offering newer releases as updates after
install — so a new upstream release needs **no** registry change. The
maintainer's merge therefore endorses the plugin repository (and its future
releases), not one reviewed binary; users confirm trust explicitly on first
install and again after every update.

> The index currently lives in the Decode-Orc repository for convenience. It is
> expected to move to a dedicated `orc-plugin-registry` repository later; the
> schema and contribution model below are written so that move is transparent
> to hosts (the fetch URL is configurable via `ORC_PLUGIN_INDEX_URL`).

## Schema (`registry_schema: 2`)

```yaml
registry_schema: 2          # index schema major version (required)
plugins:
  - id: com.example.deinterlace          # unique plugin id (required)
    display_name: Example Deinterlacer    # human-readable name
    description: Motion-adaptive deinterlacing for PAL/NTSC
    maintainer: Example Author
    license_spdx: GPL-3.0-or-later        # SPDX identifier (required)
    source_repo_url: https://github.com/example/orc-plugin_deinterlace  # required
    tags: [transform, video]
```

There is **no artifact list**: the host queries the repository's latest GitHub
release and selects the asset for its platform and ABI. (Schema 1 pinned
per-release artifacts with sha256 digests; a schema-2 entry carrying an
`artifacts` list is rejected by validation.)

### Requirements on the plugin repository

- Publish plugin binaries as **GitHub release assets** on the repository named
  by `source_repo_url`.
- Asset filenames must follow the convention
  `orc-plugin_<stage>_<platform>[-<arch>][_abi<N>].<so|dylib|dll>` —
  the host selects the asset for its platform and prefers the one tagged with
  its ABI (`_abi<N>`).
- The **latest release** is what users get: keep it working, and tag releases
  `v<version>` so the host can report and compare versions.

### Forward compatibility

- Hosts **ignore unknown fields**, so additions within schema major `2` are
  non-breaking. Older hosts tolerate a newer index and simply skip fields and
  entries they do not understand.

## Contributing

1. Fork and add your plugin's entry in `index.yaml`.
2. Ensure `source_repo_url` points at your GitHub repository and its latest
   release publishes assets following the naming convention above.
3. Open a pull request. The validation workflow
   (`.github/workflows/validate-plugin-index.yml`) checks: schema conformance,
   a valid SPDX `license_spdx`, a GitHub `source_repo_url`, and — online —
   that the repository's latest release publishes a conforming plugin asset.
4. **A maintainer's merge is the curation decision.** There is no separate
   release step; the list goes live when the PR merges, and every release you
   publish afterwards is offered to users automatically.

Only GPLv3-compatible licenses are accepted (GPLv3, GPLv2, LGPL, BSD, MIT,
Apache-2.0, ISC and similar).

## Validation locally

```bash
# Offline checks (schema, license, repository URL):
python3 tools/validate_index.py index.yaml

# Online checks as well (latest release publishes a conforming asset):
python3 tools/validate_index.py --online index.yaml
```

The fixtures under [`tests/`](tests/) pin the validator's behaviour: the good
fixtures must pass and each bad fixture must fail
(`cmake/check_plugin_index.sh` enforces this in CI and via
`ctest -R PluginIndexValidation`).
