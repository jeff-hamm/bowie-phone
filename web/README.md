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
    'voicemail'
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

## License

MIT
