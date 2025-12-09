# Setup Guide

## Prerequisites

- Google Account
- Web browser with JavaScript enabled
- For audio recording: microphone access

## Step 1: Create Google Sheet

1. Go to [Google Sheets](https://sheets.google.com)
2. Create a new spreadsheet
3. Name it "Bowie Phone Sequences" (or similar)
4. Add headers in Row 1:
   - A1: `Name`
   - B1: `Number`
   - C1: `Link`

5. Get your Sheet ID from the URL:
   ```
   https://docs.google.com/spreadsheets/d/SHEET_ID_HERE/edit#gid=0
   ```

6. Get the GID (sheet tab ID) from the URL:
   ```
   https://docs.google.com/spreadsheets/d/.../edit#gid=GID_HERE
   ```

## Step 2: Create Google Drive Folder

1. Go to [Google Drive](https://drive.google.com)
2. Create a new folder for audio recordings
3. Get the folder ID from the URL:
   ```
   https://drive.google.com/drive/folders/FOLDER_ID_HERE
   ```

## Step 3: Deploy Apps Script

1. Go to [Google Apps Script](https://script.google.com)
2. Create a new project
3. Delete the default `myFunction` code
4. Copy the contents of `src/appscript/bowie-phone-apps-script.js`
5. Paste into Code.gs
6. Create a new file (File > New > Script)
7. Name it `config` and add your real values:

```javascript
const LOCAL_CONFIG = {
  sheetId: 'your-sheet-id',
  gid: 'your-gid',
  driveFolderId: 'your-drive-folder-id'
};

function getLocalConfig() {
  return LOCAL_CONFIG;
}
```

8. Deploy:
   - Click "Deploy" > "New deployment"
   - Type: Web app
   - Execute as: Me
   - Who has access: Anyone
   - Click "Deploy"
   - Authorize when prompted
   - Copy the Web app URL

## Step 4: Configure Web App

1. Copy `src/web/config.example.js` to `src/web/config.local.js`
2. Fill in your values:

```javascript
const LOCAL_PHONE_CONFIG = {
    googleSheets: {
        enabled: true,
        spreadsheetId: 'your-sheet-id',
        gid: 'your-gid',
        appScriptUrl: 'https://script.google.com/macros/s/YOUR_DEPLOYMENT_ID/exec'
    },
    googleDrive: {
        folderId: 'your-drive-folder-id',
        folderUrl: 'https://drive.google.com/drive/folders/your-drive-folder-id'
    }
};

window.LOCAL_PHONE_CONFIG = LOCAL_PHONE_CONFIG;
```

## Step 5: Run

### Local Development
```bash
cd web/src/web
python -m http.server 8000
```
Open http://localhost:8000

### Production
Upload the `web/src/web` folder to any static hosting:
- GitHub Pages
- Netlify
- Vercel
- Any web server

## Testing

1. Open the web app
2. Check the console for "Config ready"
3. Try adding a sequence:
   - Click "Add Sequence"
   - Enter a name and number
   - Click "Add Sequence"
4. Try recording:
   - Click the record button
   - Speak into microphone
   - Stop recording
   - Click "Use" to upload

## Exporting for Phone

Run this in Apps Script to get JSON for the phone firmware:

```javascript
function exportForPhone() {
  const json = exportAsJSON();
  Logger.log(JSON.stringify(json, null, 2));
}
```

Then copy the output to your phone's `data/sequences.json` file.
