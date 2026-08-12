# Release Manifest v1.0.0

## Scope

This release marks a portfolio-ready Linux serial gateway with:
- serial ingest / protocol parse / TCP+MQTT upload
- cache + replay reliability
- downlink command channel
- heartbeat + resource metrics
- hot reload
- systemd deploy + resource limits
- ARM cross-compilation support

## Validation baseline

Required checks before publishing:

```bash
ctest --test-dir build --output-on-failure
./tests/smoke.sh
./tests/recovery.sh
./tests/command_smoke.sh
./tests/heartbeat_smoke.sh
./tests/reload_smoke.sh
```

Optional (if broker installed):

```bash
./tests/mqtt_smoke.sh
```

## Artifacts

- Source archive: `serial-gateway-v1.0.0-src.tar.gz`
- Linux x86_64 bundle: `serial-gateway-v1.0.0-linux-x86_64.tar.gz`
- Linux ARMhf bundle: `serial-gateway-v1.0.0-linux-armhf.tar.gz` (if built)
- SHA256 checksums: `SHA256SUMS`

## Binary bundle layout

```text
serial-gateway-v1.0.0-linux-<arch>/
├── bin/
│   └── serial_gateway
├── config/
│   └── gateway.yaml.example
├── systemd/
│   ├── serial-gateway.service
│   └── serial-gateway.env.example
├── scripts/
│   ├── install_systemd_service.sh
│   ├── uninstall_systemd_service.sh
│   └── set_systemd_limits.sh
└── docs/
    ├── release_notes.md
    └── demo_commands.md
```

## Tag recommendation

- Git tag: `v1.0.0`
- Message: `serial-gateway portfolio release`
