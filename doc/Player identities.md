# Per-server player identities

Kastenbrot creates a passwordless player identity for each multiplayer server. The server announces a persistent server ID, and the client selects the matching local key. A nickname may change without changing the identity.

The client stores identities in `config/player-identities.dat`. The server stores its persistent server ID and registered players in `config/server-identities.dat`. Both files are replaced through a temporary file when changed. Back up the client file: losing a private key loses access to that server identity, and anyone who obtains the file can impersonate the player. Deleting a local identity does not delete server data.

Client console commands:

- `identityinfo [server-id]` shows the server ID, permanent player ID, and public key.
- `identityexport <server-id> <file>` exports one identity, including its private key.
- `identityimport <file>` imports an exported identity.
- `identitydelete [server-id]` deletes only the local identity.
- `identityrotate <server-id>` creates a replacement key pair while retaining the player ID. A server administrator must install the displayed
  public key with `identityreplace` before the player reconnects.

Authenticated administrators can use server commands:

- `/identityrevoke <player-id>`
- `/identityreplace <player-id> <public-key>`
- `/identityban <player-id>`
- `/identityunban <player-id>`
- `/identitypermission <player-id> <integer>`

Identity authentication does not grant administrator privilege. Server passwords, administrator passwords, and master-server authentication remain separate.
