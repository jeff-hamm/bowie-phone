# Bowie Phone - Web Sequence Manager

A web-based UI for managing phone dial sequences backed by Google Sheets.

## Features

- **Sequence Management**: Create, edit, and delete phone sequences
- **Inline Editing**: Click any field to edit directly in the list
- **Audio Recording**: Record audio directly from the browser with microphone access
- **Google Drive Upload**: Recorded audio is automatically uploaded to Google Drive
- **Number Validation**: Validates dial sequences (digits, *, #) or special names
- **Real-time Sync**: Bidirectional sync with Google Sheets
- **Search**: Quick search/filter through sequences
- **Responsive Design**: Works on desktop and mobile

## Quick Start

### 1. Set Up Google Sheet

Create a Google Sheet with these columns in the first row:
| Name | Number | Link |
|------|--------|------|

Make the sheet publicly viewable (or use Apps Script for private sheets).

### 2. Set Up Google Drive Folder (for audio uploads)

1. Create a folder in Google Drive for audio files
2. Get the folder ID from the URL: `drive.google.com/drive/folders/FOLDER_ID_HERE`

### 3. Configure the Web App

Copy `config.example.js` to `config.local.js`:

```javascript
const LOCAL_PHONE_CONFIG = {
    googleSheets: {
        enabled: true,
        spreadsheetId: 'YOUR_SHEET_ID',
        gid: 'YOUR_SHEET_GID',
        appScriptUrl: 'YOUR_APPS_SCRIPT_URL'
    },
    googleDrive: {
        folderId: 'YOUR_DRIVE_FOLDER_ID',
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_DRIVE_FOLDER_ID'
    }
};
```

### 4. Deploy Google Apps Script (for write operations)

1. Open [Google Apps Script](https://script.google.com/)
2. Create a new project
3. Copy `src/appscript/bowie-phone-apps-script.js` into Code.gs
4. Create a new file and copy your local config values
5. Deploy as web app:
   - Execute as: Me
   - Who has access: Anyone
6. Copy the deployed URL to your config.local.js

### 5. Run Locally

```bash
# From the web directory
python -m http.server 8000
# Open http://localhost:8000/src/web/
```

## Number Validation

The Number field accepts:

- **Dial sequences**: Any combination of `0-9`, `*`, `#`
  - Examples: `911`, `*123#`, `411*`, `1800555`
- **Special names**: Predefined system sounds
  - `dialtone` - Standard dial tone
  - `busy` - Busy signal
  - `ringback` - Ring-back tone
  - `disconnect` - Disconnect tone
  - `error` - Error tone
  - `silence` - Silence

## Audio Recording

1. Click the red record button to start recording
2. Click again to stop
3. Preview the recording with the audio player
4. Click "Use" to upload to Google Drive
5. The URL will be automatically saved to the Link field

**Note**: Requires microphone access permission in the browser.

## Export to Phone Firmware

The Apps Script includes an `exportAsJSON()` function that outputs sequences in the format expected by the phone firmware:

```javascript
// Run in Apps Script editor
function test() {
  const json = exportAsJSON();
  console.log(JSON.stringify(json, null, 2));
}
```

Output format (matches `sample-sequence.json`):
```json
{
  "911": {
    "description": "Emergency Services",
    "type": "audio",
    "path": "https://drive.google.com/..."
  },
  "dialtone": {
    "description": "Dial Tone",
    "type": "audio",
    "path": "https://..."
  }
}
```

## File Structure

```
web/
├── src/
│   ├── web/
│   │   ├── index.html      # Main HTML
│   │   ├── app.js          # Application logic
│   │   ├── config.js       # Configuration
│   │   ├── config.example.js
│   │   └── styles/
│   │       ├── theme.css   # Theme variables
│   │       └── styles.css  # Main styles
│   └── appscript/
│       ├── bowie-phone-apps-script.js
│       └── bowie-phone-apps-script.local.example.js
├── docs/
│   └── SETUP.md
└── README.md
```

## Customization

### Theme Colors

Edit `src/web/styles/theme.css` to change the color scheme:

```css
:root {
    --color-primary: #00ff88;      /* Main accent color */
    --color-recording: #ff4444;    /* Recording indicator */
    --color-bg: #0d1117;           /* Background */
}
```

### Special Numbers

Add custom special names in `config.js`:

```javascript
specialNumbers: [
    'dialtone',
    'busy',
    'ringback',
    // Add your own...
    'hold',
    'voicemail',
    'click'
]
```

## Troubleshooting

### CORS Errors
- Ensure Google Sheet is publicly accessible, OR
- Use the Apps Script backend (which handles CORS)

### Audio Not Recording
- Check browser microphone permissions
- Ensure using HTTPS (or localhost)
- Try a different browser (Chrome recommended)

### Upload Failing
- Verify Drive folder ID is correct
- Check Apps Script deployment settings
- Ensure folder is writable

# Cookie-Based Multi-Tenant Phone System - Quick Start

## 🎯 What You Have Now

A phone sequence management system that can run multiple independent instances from one codebase, with users selecting their instance via URL.

## 🌐 The Three Sites

| Site                              | Purpose           | What It Does                                    |
| --------------------------------- | ----------------- | ----------------------------------------------- |
| **phone.infinitebutts.com**       | Main app          | Reads cookie, loads appropriate config & data   |
| **bowie-phone.infinitebutts.com** | Bowie selector    | Sets `phone-config=bowie` cookie → redirects    |
| **brophone.infinitebutts.com**    | BroPhone selector | Sets `phone-config=brophone` cookie → redirects |

## 🎯 How Users Select Config

**Method 1: Visit Redirect Sites** (Recommended)
- Visit `bowie-phone.infinitebutts.com` → Sets Bowie cookie → Redirects
- Visit `brophone.infinitebutts.com` → Sets BroPhone cookie → Redirects

**Method 2: Direct Access** (NEW!)
- Visit `phone.infinitebutts.com` with no cookie set
- See a modal with config options
- Click to select Bowie or BroPhone
- Selection is saved in cookie

## 🚀 Quick Deployment

### 1. Setup BroPhone Resources (One-Time)

Create these in Google:
- [ ] New Google Sheet (for BroPhone sequences)
- [ ] Deploy Apps Script to that sheet
- [ ] Create Drive folder (for BroPhone audio)

### 2. Update config.js

Replace these placeholders in the `'brophone'` config:
```javascript
spreadsheetId: 'YOUR_BROPHONE_SPREADSHEET_ID'
appScriptUrl: 'YOUR_BROPHONE_APPS_SCRIPT_URL' 
folderId: 'YOUR_BROPHONE_DRIVE_FOLDER_ID'
```

### 3. Deploy Files

**phone.infinitebutts.com** → Deploy entire `docs/` folder

**bowie-phone.infinitebutts.com** → Deploy ONLY `bowie-redirect.html` (rename to `index.html`)

**brophone.infinitebutts.com** → Deploy ONLY `brophone-redirect.html` (rename to `index.html`)

## ✅ Test It

1. Visit `https://bowie-phone.infinitebutts.com`
   - Should redirect to phone.infinitebutts.com
   - Should show Bowie Phone (green theme)
   
2. Visit `https://brophone.infinitebutts.com`
   - Should redirect to phone.infinitebutts.com
   - Should show BroPhone (blue theme, different data)

3. Visit `https://phone.infinitebutts.com` directly
   - Should remember your last selection

## 📋 Configuration Options

Each config can customize:

| Setting          | Bowie Default           | BroPhone Example     |
| ---------------- | ----------------------- | -------------------- |
| **Theme**        | `default` (green)       | `blue`               |
| **Google Sheet** | Your current sheet      | New BroPhone sheet   |
| **Drive Folder** | Your current folder     | New BroPhone folder  |
| **Project Name** | "Bowie Phone Sequences" | "BroPhone Sequences" |
| **Colors**       | Green/Blue              | Blue/Dark Blue       |

## 🎨 Available Themes

- `default` - Green accent colors (Bowie)
- `blue` - Blue color scheme (BroPhone)
- `dark` - Dark mode with purple accents

Create custom themes by adding `styles/theme-YOURNAME.css`

## 🔧 Admin Tools

**Config Switcher**: Deploy `config-switcher.html` for a UI to switch configs without visiting redirect sites.

Access at: `https://phone.infinitebutts.com/config-switcher.html`

## 📝 Files Created

| File                     | Purpose                   |
| ------------------------ | ------------------------- |
| `config.js`              | Core multi-tenant system  |
| `bowie-redirect.html`    | Bowie Phone selector page |
| `brophone-redirect.html` | BroPhone selector page    |
| `config-switcher.html`   | Admin config switcher     |
| `theme-blue.css`         | Blue theme for BroPhone   |
| `theme-dark.css`         | Dark theme option         |
| `COOKIE_CONFIG_SETUP.md` | Detailed technical docs   |
| `DEPLOYMENT.md`          | Full deployment guide     |
| `MULTI_TENANT_CONFIG.md` | Configuration reference   |

## 🔍 Troubleshooting

**See wrong data?**
- Check cookie in DevTools → Application → Cookies
- Verify spreadsheet IDs are different in config.js

**Cookie not sticking?**
- Check domain is `.infinitebutts.com` (with the dot)
- Verify cookies are enabled in browser

**Redirect loop?**
- Make sure phone.infinitebutts.com doesn't have redirect config
- Verify redirect sites only have the redirect HTML

## 📚 Documentation

- **[COOKIE_CONFIG_SETUP.md](COOKIE_CONFIG_SETUP.md)** - How the cookie system works
- **[DEPLOYMENT.md](DEPLOYMENT.md)** - Step-by-step deployment guide
- **[MULTI_TENANT_CONFIG.md](MULTI_TENANT_CONFIG.md)** - URL-based configuration docs

## 🎯 Next Steps

1. ✅ Set up BroPhone Google Sheet and Drive folder
2. ✅ Update the placeholder IDs in config.js
3. ✅ Deploy to your three subdomains
4. ✅ Test both configurations
5. ✅ Share the URLs with users

## 💡 Tips

- Cookie lasts 365 days - users won't need to re-select often
- Each config can have completely separate data
- Add more configs anytime by following the same pattern
- Use config-switcher.html for easy testing during development

## License

MIT
