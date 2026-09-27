# Replacing the release signing key

The public key compiled into `sonora-updater` is a **development key**: its private half was
generated on a machine that is not a secret store, and it has been in this repository's
history. It is not a signing key, and the release job knows it — there is a check that greps
for its fingerprint and refuses to publish a manifest while the binary still trusts it,
because a note in a comment is not a check.

Replacing it is four steps, and the last one is the one people skip.

## 1. Make a real pair

```powershell
./tools/update_keygen.ps1
```

It prints the **public** half as a C++ array ready to paste, and writes the **private** half
to a PEM file **outside this repository** — under `%TEMP%` — whose path it tells you. Outside
on purpose: a default that cannot go wrong beats a rule that catches it going wrong, and
`.gitignore` refusing `*.pem` is the second lock on that door rather than the first. It also prints the two commands that follow, which are
the ones in step 3.

The private half is now a file on your disk. It stops being a secret the moment it is
anywhere else, so do not paste it into a chat, an editor with a recovery file, or a terminal
that logs — and delete the file as soon as the secret is set.

## 2. Put the public half in the binary

The script prints a `constexpr PublicKey kReleaseKey2026 = { … };` block. In
`src/update/src/signature.cpp`, **replace `kDevelopmentKey` entirely** with it — replace, not
append, and update the list of keys `ReleaseKeys()` returns so the old one is not still
trusted alongside the new one.

The release job greps for the first four bytes of the development key
(`0x90, 0x77, 0x2d, 0x19`), so leaving any part of that array behind keeps the build refusing
to publish. That is the desired behaviour: it is what tells you if you half-did it.

Commit that. It is a change to what every installation trusts, and it deserves its own commit
message saying so.

## 3. Put the private half in the repository secret

The script prints both commands with the real path filled in:

```powershell
Get-Content -Raw "$env:TEMP\sonora-release-key.pem" | gh secret set SONORA_RELEASE_KEY
Remove-Item "$env:TEMP\sonora-release-key.pem"
```

`Get-Content` and a pipe rather than `<`: PowerShell reserves `<` and refuses the line
before running anything, which is the sort of detail that only shows up when somebody
actually types it.

The second line is not optional and is not tidiness: a private key that is still on a laptop
is a private key that will be in a backup.

This is weaker than an offline key and the trade is written down in
[ADR 0009](adr/0009-the-update-server-is-a-signed-file.md): **whoever can run a
workflow in this repository can sign a release.** For a project of this size that is the right
trade; it is not the right trade for a product, and the ADR says which part would have to
change.

## 4. Check that it actually signed something

This is the step that gets skipped, and the failure is silent in the worst way — a release
that looks complete and is missing the one file that makes updates work.

After the release run finishes:

```powershell
gh release view v1.0.0 --json assets --jq '.assets[].name'
```

`manifest.json` **and** `manifest.json.sig` must both be there. If they are not, the job put a
warning in its log saying which of the two checks stopped it: no key in the secret, or the
binary still trusting the development key.

And then verify it the way a client will, rather than trusting the job that made it:

```powershell
gh release download v1.0.0 --pattern 'manifest.json*'
# the same two files an installation fetches, checked with the public half from the binary
```

The end-to-end test in `tests/test_update_e2e.cpp` does this against a signature produced by
the exact same OpenSSL command the release job runs — which is how we know that OpenSSL
signing on a runner and libsodium verifying on somebody's laptop agree about Ed25519.

## What this does not fix

Code signing for the MSI is a different key and a different problem. `tools/sign.ps1` demonstrates the pipeline with a self-signed certificate, and a self-signed certificate is
trusted by exactly one machine — SmartScreen will still warn, and Smart App Control will still
refuse. A real release needs an OV or EV certificate from a CA, with the private half in an
HSM or a cloud signing service. That is money and identity verification, not code.
