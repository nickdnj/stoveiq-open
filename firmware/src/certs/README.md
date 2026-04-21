# TLS Certificates

This directory holds the self-signed TLS cert + key that the ESP32 serves
over HTTPS on port 443 (for `stoveiq.local`).

**Nothing in this directory is committed to git.** Run the generator to
populate it before your first build:

```bash
./scripts/gen-cert.sh
```

That creates:

- `server.crt` — public self-signed cert (CN=stoveiq.local, 10-year validity)
- `server.key` — private RSA-2048 key (git-ignored; never share)
- `../certs.h` — embedded PEM strings consumed by `web_server.c`

Re-run `gen-cert.sh` to rotate the cert. Each device you access from will
show Safari's "Connection Not Private" warning once — tap **Show Details →
Visit Website** and it's remembered.

Edit the `SAN` list in `gen-cert.sh` if your home network IPs differ.
