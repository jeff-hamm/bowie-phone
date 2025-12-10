// Bowie Phone Sequence Manager - Local Configuration Example
// COPY this file to `config.local.js` and insert your real values.
// Ensure `.gitignore` has an entry to ignore `config.local.js` so secrets are not committed.

const LOCAL_PHONE_CONFIG = {
    googleSheets: {
        spreadsheetId: 'YOUR_SPREADSHEET_ID_HERE'
    },
    googleDrive: {
        folderId: 'YOUR_DRIVE_FOLDER_ID_HERE',
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_DRIVE_FOLDER_ID_HERE'
    }
};

// Export for merging with main config
window.LOCAL_PHONE_CONFIG = LOCAL_PHONE_CONFIG;
