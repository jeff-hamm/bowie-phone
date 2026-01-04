# BroPhone Setup Checklist

Use this checklist to complete your BroPhone configuration.

## ☑️ Pre-Deployment Tasks

### 1. Create BroPhone Google Sheet
- [ ] Go to Google Drive
- [ ] Create new Google Sheet
- [ ] Name it "BroPhone Sequences" (or your preferred name)
- [ ] Set up columns matching your Bowie Phone sheet:
  - Number
  - Link Type  
  - Link/Command
  - Description
  - (Any other columns you use)
- [ ] Copy the Spreadsheet ID from the URL
  - URL looks like: `https://docs.google.com/spreadsheets/d/SPREADSHEET_ID/edit`
  - Copy the `SPREADSHEET_ID` part

**Spreadsheet ID**: _______________________________________

### 2. Deploy Apps Script for BroPhone
- [ ] Open your BroPhone Google Sheet
- [ ] Click Extensions → Apps Script
- [ ] Delete any default code
- [ ] Copy code from: `submodules/to-do/src/appscript/universal-apps-script.js`
- [ ] Paste into Apps Script editor
- [ ] Click Deploy → New deployment
- [ ] Select type: Web app
- [ ] Execute as: Me
- [ ] Who has access: Anyone
- [ ] Click Deploy
- [ ] Copy the Web app URL (should end with `/exec`)

**Apps Script URL**: _______________________________________

### 3. Create BroPhone Drive Folder
- [ ] Go to Google Drive
- [ ] Create new folder
- [ ] Name it "BroPhone Audio Files" (or your preferred name)
- [ ] Right-click → Share → Anyone with the link can view
- [ ] Copy the folder ID from URL
  - URL looks like: `https://drive.google.com/drive/folders/FOLDER_ID`
  - Copy the `FOLDER_ID` part

**Drive Folder ID**: _______________________________________

### 4. Update config-manager.js
- [ ] Open `docs/config-manager.js`
- [ ] Find the `'brophone'` configuration (around line 48)
- [ ] Replace `YOUR_BROPHONE_SPREADSHEET_ID` with your Spreadsheet ID from step 1
- [ ] Replace `YOUR_BROPHONE_APPS_SCRIPT_URL` with your Apps Script URL from step 2
- [ ] Replace `YOUR_BROPHONE_DRIVE_FOLDER_ID` with your folder ID from step 3 (appears twice)
- [ ] Save the file

Example:
```javascript
'brophone': {
    googleSheets: {
        spreadsheetId: '1ABCxyz123...',  // Your ID here
        appScriptUrl: 'https://script.google.com/macros/s/ABC123.../exec'
    },
    googleDrive: {
        folderId: '1XYZ789...',  // Your ID here
        folderUrl: 'https://drive.google.com/drive/folders/1XYZ789...'
    }
}
```

## 🚀 Deployment Tasks

### 5. Deploy Main App (phone.infinitebutts.com)
- [ ] Upload entire `docs/` folder to phone.infinitebutts.com
- [ ] Verify all files uploaded:
  - index.html
  - app.js
  - config-manager.js (with your updates!)
  - universal-sheet-api.js
  - styles/ directory (all CSS files)
- [ ] Test: Visit https://phone.infinitebutts.com
- [ ] Should load without errors

### 6. Deploy Bowie Redirect (bowie-phone.infinitebutts.com)
- [ ] Copy `docs/bowie-redirect.html`
- [ ] Rename to `index.html`
- [ ] Upload to bowie-phone.infinitebutts.com
- [ ] Test: Visit https://bowie-phone.infinitebutts.com
- [ ] Should show green loading screen
- [ ] Should redirect to phone.infinitebutts.com
- [ ] Should load Bowie Phone configuration

### 7. Deploy BroPhone Redirect (brophone.infinitebutts.com)
- [ ] Copy `docs/brophone-redirect.html`
- [ ] Rename to `index.html`
- [ ] Upload to brophone.infinitebutts.com
- [ ] Test: Visit https://brophone.infinitebutts.com
- [ ] Should show blue loading screen
- [ ] Should redirect to phone.infinitebutts.com
- [ ] Should load BroPhone configuration

## ✅ Testing Tasks

### 8. Test Cookie Functionality
- [ ] Open browser DevTools (F12)
- [ ] Go to Application tab → Cookies
- [ ] Visit https://bowie-phone.infinitebutts.com
- [ ] Verify cookie `phone-config` = `bowie` is set
- [ ] Visit https://brophone.infinitebutts.com
- [ ] Verify cookie `phone-config` = `brophone` is set

### 9. Test Configuration Switching
- [ ] Clear all cookies for .infinitebutts.com
- [ ] Visit https://bowie-phone.infinitebutts.com
- [ ] Verify:
  - [ ] Green/default theme loads
  - [ ] Page title is "Bowie Phone Sequences"
  - [ ] Connects to Bowie Google Sheet
- [ ] Visit https://brophone.infinitebutts.com  
- [ ] Verify:
  - [ ] Blue theme loads
  - [ ] Page title is "BroPhone Sequences"
  - [ ] Connects to BroPhone Google Sheet (different data)

### 10. Test Direct Access
- [ ] With BroPhone cookie set, close all tabs
- [ ] Visit https://phone.infinitebutts.com directly
- [ ] Should remember BroPhone config (blue theme, BroPhone data)
- [ ] Switch back to Bowie: visit https://bowie-phone.infinitebutts.com
- [ ] Close all tabs
- [ ] Visit https://phone.infinitebutts.com directly
- [ ] Should remember Bowie config

### 11. Test Data Isolation
- [ ] Set cookie to Bowie
- [ ] Add a test sequence to Bowie Phone
- [ ] Switch to BroPhone
- [ ] Verify test sequence doesn't appear (different sheet)
- [ ] Add a test sequence to BroPhone
- [ ] Switch back to Bowie
- [ ] Verify BroPhone sequence doesn't appear

## 🎨 Optional Customization

### 12. Customize BroPhone Theme (Optional)
- [ ] Edit `docs/config-manager.js`
- [ ] Find `'brophone'` config
- [ ] Modify `customStyles`:
  ```javascript
  customStyles: {
      primaryColor: '#YOUR_COLOR',
      accentColor: '#YOUR_COLOR'
  }
  ```
- [ ] Or change theme: `theme: "dark"` (for dark mode)

### 13. Deploy Config Switcher (Optional)
- [ ] Upload `docs/config-switcher.html` to phone.infinitebutts.com
- [ ] Access at: https://phone.infinitebutts.com/config-switcher.html
- [ ] Test switching configs without visiting redirect sites

## 🔍 Troubleshooting

If something doesn't work:

**Console shows config errors:**
- [ ] Check you replaced ALL placeholder IDs in config-manager.js
- [ ] Verify spreadsheet ID is correct (check URL)
- [ ] Verify Apps Script URL ends with `/exec`

**Cookie not persisting:**
- [ ] Check cookie domain is `.infinitebutts.com` (with dot)
- [ ] Verify browser allows cookies
- [ ] Try clearing all cookies and starting fresh

**Wrong data loads:**
- [ ] Check cookie value in DevTools
- [ ] Verify different spreadsheet IDs in config-manager.js
- [ ] Check Network tab to see which sheet URL is called
- [ ] Clear browser cache

**Theme not changing:**
- [ ] Verify `styles/theme-blue.css` exists
- [ ] Check browser console for 404 errors
- [ ] Hard refresh (Ctrl+Shift+R or Cmd+Shift+R)

## ✨ Done!

Once all checkboxes are complete, your multi-tenant phone system is ready!

Users can now:
- Visit bowie-phone.infinitebutts.com to use Bowie Phone
- Visit brophone.infinitebutts.com to use BroPhone
- Bookmark phone.infinitebutts.com and their choice will be remembered

---

**Need help?** Check these docs:
- [QUICKSTART.md](QUICKSTART.md) - Quick overview
- [DEPLOYMENT.md](DEPLOYMENT.md) - Detailed deployment guide  
- [COOKIE_CONFIG_SETUP.md](COOKIE_CONFIG_SETUP.md) - How cookies work
- [ARCHITECTURE.md](ARCHITECTURE.md) - System architecture
