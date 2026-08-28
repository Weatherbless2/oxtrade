# Project Boundaries

## oxApp

`oxApp` is the desktop client. It will own user interaction, presentation,
client-side logging, and the conversion of approved backend messages into UI
state. UI changes must remain on the Qt main thread.

## oxMBS

`oxMBS` will be a separate business-service platform. It will own service
supervision, routing, worker scheduling, `funclist.xml`, and dynamic LBM
execution. oxApp must not call its database or business modules directly.

## Other planned services

- `oxMWH`: this repository's market-data ingestion core and normalized quote delivery; formal broker/HP-Socket adapters are added behind its interfaces.
- `lbm_sys`, `lbm_right`, `lbm_fund`: loadable business modules called by
  oxMBS workers.
- Trading gateway and query services: isolated backend processes. oxApp does
  not connect to a broker or counter in this initial project baseline.

## Configuration and secrets

Only `.example` configuration files are versioned. Runtime configuration,
credentials, private keys, logs, and build output stay local.
