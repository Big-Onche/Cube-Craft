# Per-server player identities

Kastenbrot creates a passwordless player identity for each multiplayer server. The server announces a persistent server ID, and the client selects the matching local key. A nickname may change without changing the identity.

The client stores identities in `config/player-identities.dat`. The server stores its persistent server ID and registered players in `config/server-identities.dat`. Both files are replaced through a temporary file when changed. Back up the client file: losing a private key loses access to that server identity, and anyone who obtains the file can impersonate the player. Deleting a local identity does not delete server data.

Client console commands:

- `idinfo [server-id]` (or `identityinfo`) shows the server ID, permanent player ID, and public key.
- `idexport <server-id> <file>` (or `identityexport`) exports one identity, including its private key.
- `idimport <file>` (or `identityimport`) imports an exported identity.
- `iddelete [server-id]` (or `identitydelete`) deletes only the local identity.
- `idrotate <server-id>` (or `identityrotate`) creates a replacement key pair while retaining the player ID. A server administrator must install the displayed public key with `idreplace` before the player reconnects.

Authenticated administrators can use server commands:

- `/idrevoke <player-id>` (or `/identityrevoke`)
- `/idreplace <player-id> <public-key>` (or `/identityreplace`)
- `/idban <player-id>` (or `/identityban`)
- `/idunban <player-id>` (or `/identityunban`)
- `/idpermission <player-id> <integer>` (or `/identitypermission`)

Identity authentication does not grant administrator privilege. Server passwords, administrator passwords, and master-server authentication remain separate.
