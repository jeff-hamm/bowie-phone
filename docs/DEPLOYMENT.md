# Deployment Guide for infinitebutts.com

This guide explains how to deploy the multi-site phone configuration system.

## Overview

You'll be deploying to three different subdomains:

1. **phone.infinitebutts.com** - Main application
2. **bowie-phone.infinitebutts.com** - Redirect site for Bowie Phone
3. **brophone.infinitebutts.com** - Redirect site for BroPhone

## Before You Start

### 1. Set Up BroPhone Google Resources

Create separate Google resources for BroPhone:

1. **Google Sheet**: Create a new sheet for BroPhone sequences
   - Copy the structure from your Bowie Phone sheet
   - Note the Spreadsheet ID from the URL

2. **Apps Script**: Deploy the universal backend
   - Open the new sheet → Extensions → Apps Script
   - Copy the code from `universal-apps-script.js`
   - Deploy as Web App
   - Copy the deployment URL

3. **Google Drive**: Create a folder for BroPhone audio
   - Create a new folder in Google Drive
   - Note the folder ID from the URL

### 2. Update config-manager.js

Replace the placeholder values in [config-manager.js](config-manager.js):

```javascript
'brophone': {
    googleSheets: {
        spreadsheetId: 'YOUR_BROPHONE_SPREADSHEET_ID',  // Replace
        appScriptUrl: 'YOUR_BROPHONE_APPS_SCRIPT_URL'   // Replace
    },
    googleDrive: {
        folderId: 'YOUR_BROPHONE_DRIVE_FOLDER_ID',      // Replace
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_BROPHONE_DRIVE_FOLDER_ID'
    }
}
```

## Deployment Steps

### Site 1: phone.infinitebutts.com (Main App)

Deploy all files from the `docs/` directory:

```
docs/
├── index.html
├── app.js
├── config-manager.js
├── universal-sheet-api.js
├── config-switcher.html (optional, for admin)
├── styles/
│   ├── styles.css
│   ├── theme.css
│   ├── theme-blue.css
│   └── theme-dark.css
└── assets/ (if any)
```

**Testing**: Visit `https://phone.infinitebutts.com` and verify it loads (should use default/bowie config if no cookie is set).

### Site 2: bowie-phone.infinitebutts.com (Redirect)

Deploy only ONE file:

1. Copy `docs/bowie-redirect.html`
2. Rename it to `index.html`
3. Upload to `bowie-phone.infinitebutts.com`

**Testing**: 
1. Visit `https://bowie-phone.infinitebutts.com`
2. Should see "Setting up Bowie Phone..." message
3. Should redirect to `phone.infinitebutts.com` within 1 second
4. Check DevTools → Application → Cookies: should see `phone-config=bowie`

### Site 3: brophone.infinitebutts.com (Redirect)

Deploy only ONE file:

1. Copy `docs/brophone-redirect.html`
2. Rename it to `index.html`
3. Upload to `brophone.infinitebutts.com`

**Testing**:
1. Visit `https://brophone.infinitebutts.com`
2. Should see "Setting up BroPhone..." message (blue theme)
3. Should redirect to `phone.infinitebutts.com` within 1 second
4. Check DevTools → Application → Cookies: should see `phone-config=brophone`

## Verification Checklist

### Main Site (phone.infinitebutts.com)

- [ ] Site loads without errors
- [ ] Console shows: `🌐 Loading configuration for: phone.infinitebutts.com`
- [ ] If no cookie: loads Bowie Phone (default) configuration
- [ ] If cookie set: loads appropriate configuration
- [ ] Theme changes based on configuration
- [ ] Google Sheets integration works

### Bowie Redirect (bowie-phone.infinitebutts.com)

- [ ] Page displays "Bowie Phone" branding (green gradient)
- [ ] Sets cookie: `phone-config=bowie`
- [ ] Redirects to phone.infinitebutts.com
- [ ] Main site loads Bowie configuration

### BroPhone Redirect (brophone.infinitebutts.com)

- [ ] Page displays "BroPhone" branding (blue gradient)
- [ ] Sets cookie: `phone-config=brophone`
- [ ] Redirects to phone.infinitebutts.com
- [ ] Main site loads BroPhone configuration
- [ ] Different sheet/data loads
- [ ] Blue theme is applied

## User Flow Testing

### Test 1: Fresh User → Bowie
1. Clear all cookies for `.infinitebutts.com`
2. Visit `https://bowie-phone.infinitebutts.com`
3. Should redirect and load Bowie Phone config
4. Verify cookie is set
5. Close tab and reopen `https://phone.infinitebutts.com`
6. Should still load Bowie Phone config (cookie persists)

### Test 2: Switch to BroPhone
1. Visit `https://brophone.infinitebutts.com`
2. Should redirect and load BroPhone config (blue theme)
3. Verify different data/sheet loads
4. Cookie should now be `phone-config=brophone`

### Test 3: Direct Access
1. With cookie already set, visit `https://phone.infinitebutts.com` directly
2. Should load the configuration matching the cookie
3. No redirect should occur

## Troubleshooting

### Cookie Not Persisting

**Problem**: Cookie is set but lost after closing browser

**Solution**: Check cookie settings in the redirect HTML files. They should include:
```javascript
domain=.infinitebutts.com  // Note the leading dot
```

### Redirect Loop

**Problem**: Site keeps redirecting endlessly

**Solution**: 
- Verify `phone.infinitebutts.com` is NOT configured to redirect
- Check that redirect sites only have the redirect HTML, not the full app

### Wrong Configuration Loading

**Problem**: Cookie is set but wrong config loads

**Solution**:
1. Check cookie value: Open DevTools → Application → Cookies
2. Verify cookie name is exactly `phone-config`
3. Verify cookie value matches a config in `SITE_CONFIGS` (`bowie` or `brophone`)
4. Check console for config loading messages

### BroPhone Shows Bowie Data

**Problem**: BroPhone loads but shows Bowie Phone data

**Solution**:
1. Verify you updated the spreadsheet ID in `config-manager.js`
2. Clear browser cache
3. Check Network tab to see which sheet URL is being called
4. Verify the Apps Script URL is correct and deployed

## Optional: Admin Tools

### Config Switcher

Deploy `config-switcher.html` to:
- `https://phone.infinitebutts.com/config-switcher.html`
- Or `https://admin.infinitebutts.com` for easy access

This provides a UI for switching configurations without visiting redirect sites.

### Add to Main App

Optionally add a config switcher link to the main app header (in `index.html`):

```html
<a href="config-switcher.html" class="config-link">⚙️ Switch Config</a>
```

## DNS Configuration

Ensure your DNS has these A/CNAME records:

```
phone.infinitebutts.com      → Your server IP
bowie-phone.infinitebutts.com → Your server IP  
brophone.infinitebutts.com   → Your server IP
```

Or if using a CDN/hosting service, point all three to the same deployment with different paths.

## Security Notes

- Cookies use `SameSite=Lax` for security
- Cookie domain is `.infinitebutts.com` (all subdomains)
- Cookies expire after 365 days
- No sensitive data is stored in cookies (only config selection)

## Maintenance

### Adding a New Configuration

1. Add config to `SITE_CONFIGS` in `config-manager.js`
2. Create redirect HTML file
3. Deploy redirect site
4. Update this documentation
5. (Optional) Add theme CSS file if needed

### Removing a Configuration

1. Remove from `SITE_CONFIGS`
2. Take down redirect site
3. Users with that cookie will fall back to default
