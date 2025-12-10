// Bowie Phone Sequence Manager - Local Configuration
// This file contains environment-specific settings and is not committed to git

const LOCAL_PHONE_CONFIG = {
    // Google Sheets Integration - spreadsheet ID for this project
    googleSheets: {
        spreadsheetId: '1q1FOzSTg-5ATMSlVf_cuIaPvYgMv9yDec4Nd5ZaAlzY'
    },
    
    // Google Drive folder for audio uploads
    googleDrive: {
        folderId: '1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5',
        folderUrl: 'https://drive.google.com/drive/folders/1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5'
    }
};

// Export for merging with main config
window.LOCAL_PHONE_CONFIG = LOCAL_PHONE_CONFIG;
