# System Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Cookie-Based Multi-Tenant System                │
└─────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                          USER FLOW                                   │
└──────────────────────────────────────────────────────────────────────┘

┌────────────────────────┐
│  User visits redirect  │
│  site to select config │
└───────────┬────────────┘
            │
            ├─────────────────────────────────┐
            │                                 │
            ▼                                 ▼
┌──────────────────────────┐    ┌──────────────────────────┐
│ bowie-phone              │    │ brophone                 │
│ .infinitebutts.com       │    │ .infinitebutts.com       │
│                          │    │                          │
│ bowie-redirect.html      │    │ brophone-redirect.html   │
├──────────────────────────┤    ├──────────────────────────┤
│ 1. Shows loading screen  │    │ 1. Shows loading screen  │
│ 2. Sets cookie:          │    │ 2. Sets cookie:          │
│    phone-config=bowie    │    │    phone-config=brophone │
│ 3. Redirects ─────────┐  │    │ 3. Redirects ─────────┐  │
└──────────────────────┼───┘    └──────────────────────┼───┘
                       │                               │
                       └───────────┬───────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────────┐
                    │  phone.infinitebutts.com     │
                    │                              │
                    │  index.html + app.js         │
                    ├──────────────────────────────┤
                    │  1. Loads config-manager.js  │
                    │  2. Reads phone-config       │
                    │     cookie                   │
                    │  3. Loads appropriate        │
                    │     configuration            │
                    │  4. Applies theme            │
                    │  5. Connects to Google       │
                    │     Sheet & Drive            │
                    └──────────────────────────────┘
                                   │
                    ┌──────────────┴──────────────┐
                    │                             │
                    ▼                             ▼
        ┌──────────────────────┐    ┌──────────────────────┐
        │ BOWIE CONFIGURATION  │    │ BROPHONE CONFIG      │
        ├──────────────────────┤    ├──────────────────────┤
        │ Theme: Default/Green │    │ Theme: Blue          │
        │ Sheet: 1q1FOz...     │    │ Sheet: YOUR_SHEET    │
        │ Drive: 1TGRbk...     │    │ Drive: YOUR_FOLDER   │
        │ Name: Bowie Phone    │    │ Name: BroPhone       │
        └──────────────────────┘    └──────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                      CONFIGURATION LOADING                           │
└──────────────────────────────────────────────────────────────────────┘

┌─────────────────────────┐
│ config-manager.js loads │
└───────────┬─────────────┘
            │
            ▼
┌──────────────────────────────────┐
│ 1. Check if redirect site?       │ ───Yes───> Set cookie & redirect
│    (bowie-phone/brophone)         │
└────────────┬─────────────────────┘
             │ No
             ▼
┌──────────────────────────────────┐
│ 2. Read phone-config cookie      │
└────────────┬─────────────────────┘
             │
      ┌──────┴──────┐
      │             │
    Found        Not Found
      │             │
      ▼             ▼
┌──────────┐   ┌──────────┐
│ Load     │   │ Check    │
│ cookie   │   │ hostname │
│ config   │   │ match    │
└──────────┘   └────┬─────┘
      │             │
      │       ┌─────┴─────┐
      │     Found      Not Found
      │       │            │
      │       ▼            ▼
      │  ┌──────────┐  ┌──────────┐
      │  │ Load     │  │ Use      │
      │  │ hostname │  │ default  │
      │  │ config   │  │ config   │
      │  └──────────┘  └──────────┘
      │       │            │
      └───────┴────────────┴────────>
                  │
                  ▼
        ┌─────────────────────┐
        │ Apply configuration │
        ├─────────────────────┤
        │ - Set theme         │
        │ - Update title      │
        │ - Set CSS vars      │
        │ - Export config     │
        └─────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                         DATA FLOW                                    │
└──────────────────────────────────────────────────────────────────────┘

Main App (phone.infinitebutts.com)
        │
        ├─ Bowie Config? ───> Google Sheet A
        │                     └─> Drive Folder A
        │                         └─> Theme: Green
        │
        └─ BroPhone Config? ─> Google Sheet B
                              └─> Drive Folder B
                                  └─> Theme: Blue

┌──────────────────────────────────────────────────────────────────────┐
│                     COOKIE DETAILS                                   │
└──────────────────────────────────────────────────────────────────────┘

Name:     phone-config
Value:    "bowie" | "brophone"
Domain:   .infinitebutts.com (all subdomains)
Path:     / (entire site)
Expires:  365 days from set
SameSite: Lax (secure, allows navigation)

┌──────────────────────────────────────────────────────────────────────┐
│                    DEPLOYMENT STRUCTURE                              │
└──────────────────────────────────────────────────────────────────────┘

bowie-phone.infinitebutts.com/
└── index.html (renamed from bowie-redirect.html)

brophone.infinitebutts.com/
└── index.html (renamed from brophone-redirect.html)

phone.infinitebutts.com/
├── index.html
├── app.js
├── config-manager.js
├── universal-sheet-api.js
├── config-switcher.html (optional)
└── styles/
    ├── styles.css
    ├── theme.css
    ├── theme-blue.css
    └── theme-dark.css

┌──────────────────────────────────────────────────────────────────────┐
│                      ADMIN TOOLS                                     │
└──────────────────────────────────────────────────────────────────────┘

Option 1: Use redirect sites
  Visit bowie-phone.infinitebutts.com or brophone.infinitebutts.com

Option 2: Use config switcher
  Visit phone.infinitebutts.com/config-switcher.html
  ┌─────────────────────────────┐
  │ Select Config:              │
  │ ○ Bowie Phone               │
  │ ○ BroPhone                  │
  │ [Apply] [Clear Cookie]      │
  └─────────────────────────────┘

Option 3: Manual cookie (DevTools Console)
  document.cookie = 'phone-config=bowie;path=/;max-age=31536000'
  document.cookie = 'phone-config=brophone;path=/;max-age=31536000'
```
