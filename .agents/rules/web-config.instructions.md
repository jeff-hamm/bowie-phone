---
description: "Use when reading, writing, or debugging the web app: config loading, multi-tenant cookie system, Google Sheet/Drive integration, redirect pages, or deployment structure."
applyTo: "docs/**/*.js,docs/**/*.html,docs/**/*.css"
---
# Bowie Phone — Web App Architecture

## Multi-Tenant Config System (Cookie-Based)

The web app at `phone.infinitebutts.com` supports multiple phone configurations selected via a cookie.

**Cookie details:**
- Name: `phone-config`
- Values: `"bowie"` | `"brophone"`
- Domain: `.infinitebutts.com` (all subdomains)
- Expiry: 365 days; SameSite: Lax

### Config Resolution Order (in `config-manager.js`)

1. Is this a redirect subdomain (`bowie-phone.*` or `brophone.*`)? → set cookie + redirect to `phone.infinitebutts.com`.
2. Read `phone-config` cookie → load matching config.
3. No cookie → match hostname → load hostname config.
4. No match → load default config.

### Available Configurations

| Cookie value | Theme | Name |
|---|---|---|
| `bowie` | Default/Green | Bowie Phone |
| `brophone` | Blue | BroPhone |

Sheet IDs, Drive folder IDs, and per-tenant details are defined in `config.js` (see `config.example.js` for the template).

## File Responsibilities

| File | Responsibility |
|---|---|
| `config-manager.js` | Cookie read/write, config selection, theme application |
| `config.js` | Named config definitions (sheet IDs, Drive folder IDs, themes) |
| `config.example.js` | Template — do not edit directly; copy to `config.js` |
| `universal-sheet-api.js` | Google Sheets/Drive API wrapper |
| `app.js` | Main app logic; consumes config exported by `config-manager.js` |
| `dialer.js` | Dial-pad UI and DTMF input |
| `bowie-redirect.html` | Sets `phone-config=bowie` cookie, then redirects |
| `brophone-redirect.html` | Sets `phone-config=brophone` cookie, then redirects |
| `config-switcher.html` | Admin tool: manual cookie selection without redirect sites |

## Deployment Structure

```
bowie-phone.infinitebutts.com/   → bowie-redirect.html (deployed as index.html)
brophone.infinitebutts.com/      → brophone-redirect.html (deployed as index.html)

phone.infinitebutts.com/
├── index.html
├── app.js
├── config-manager.js
├── universal-sheet-api.js
├── config-switcher.html
└── styles/
    ├── styles.css
    ├── theme.css
    ├── theme-blue.css
    └── theme-dark.css
```

## Rules for Working with This Code

- **Config values live in `config.js`**, never hardcoded in `app.js` or `dialer.js`.
- Adding a new tenant requires: a new entry in `config.js`, a new redirect HTML page, and a corresponding DNS subdomain.
- `config-manager.js` must remain the single place that reads/writes the `phone-config` cookie — do not duplicate cookie logic elsewhere.
- Theme switching is done by applying CSS variable overrides; do not hardcode colours in component files.
- To switch config manually (e.g., in DevTools): `document.cookie = 'phone-config=bowie;path=/;max-age=31536000'`
