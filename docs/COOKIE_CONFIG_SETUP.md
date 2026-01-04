# Cookie-Based Configuration Setup

This setup allows users to select which configuration to use by visiting different URLs that set cookies and redirect to the main site.

## How It Works

1. **Redirect Sites**: `bowie-phone.infinitebutts.com` and `brophone.infinitebutts.com` set cookies and redirect
2. **Main Site**: `phone.infinitebutts.com` reads the cookie and loads the appropriate configuration
3. **Cookie Persistence**: The configuration cookie lasts for 365 days

## Site Structure

```
bowie-phone.infinitebutts.com  → Sets cookie "bowie"    → Redirects to phone.infinitebutts.com
brophone.infinitebutts.com     → Sets cookie "brophone" → Redirects to phone.infinitebutts.com
phone.infinitebutts.com        → Reads cookie           → Loads appropriate config
```

## Deployment

### 1. Main Site (phone.infinitebutts.com)

Deploy the full application:
- `index.html`
- `app.js`
- `config-manager.js`
- `universal-sheet-api.js`
- `styles/` directory
- All other assets

### 2. Bowie Phone Site (bowie-phone.infinitebutts.com)

Deploy only:
- `bowie-redirect.html` (rename to `index.html`)

### 3. BroPhone Site (brophone.infinitebutts.com)

Deploy only:
- `brophone-redirect.html` (rename to `index.html`)

## Configuration

The configurations are defined in [config-manager.js](config-manager.js):

### Bowie Phone Config (Default)
```javascript
'bowie': {
    projectName: "Bowie Phone Sequences",
    theme: "default",
    googleSheets: {
        spreadsheetId: '1q1FOzSTg-5ATMSlVf_cuIaPvYgMv9yDec4Nd5ZaAlzY',
        appScriptUrl: 'YOUR_BOWIE_SCRIPT_URL'
    }
}
```

### BroPhone Config
```javascript
'brophone': {
    projectName: "BroPhone Sequences",
    theme: "blue",
    googleSheets: {
        spreadsheetId: 'YOUR_BROPHONE_SPREADSHEET_ID',
        appScriptUrl: 'YOUR_BROPHONE_SCRIPT_URL'
    }
}
```

## Setting Up BroPhone

To complete the BroPhone setup:

1. **Create a Google Sheet** for BroPhone sequences
2. **Deploy the Apps Script** to that sheet
3. **Create a Drive folder** for BroPhone audio files
4. **Update config-manager.js** with the IDs:

```javascript
'brophone': {
    googleSheets: {
        spreadsheetId: 'YOUR_ACTUAL_SHEET_ID',     // Replace this
        appScriptUrl: 'YOUR_ACTUAL_SCRIPT_URL'     // Replace this
    },
    googleDrive: {
        folderId: 'YOUR_ACTUAL_FOLDER_ID',         // Replace this
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_ACTUAL_FOLDER_ID'
    }
}
```

## User Flow

### First Time Visitor
1. User visits `bowie-phone.infinitebutts.com`
2. Cookie is set: `phone-config=bowie`
3. Redirected to `phone.infinitebutts.com`
4. Main site loads Bowie Phone configuration

### Switching Configurations
1. User visits `brophone.infinitebutts.com`
2. Cookie is updated: `phone-config=brophone`
3. Redirected to `phone.infinitebutts.com`
4. Main site loads BroPhone configuration (blue theme, different sheet)

### Returning Visitor
1. User visits `phone.infinitebutts.com` directly
2. Cookie is read
3. Appropriate configuration loads automatically

## Cookie Details

- **Name**: `phone-config`
- **Values**: `bowie` or `brophone`
- **Duration**: 365 days
- **Domain**: `.infinitebutts.com` (works across all subdomains)
- **Path**: `/` (works across entire site)
- **SameSite**: `Lax` (secure but allows normal navigation)

## Testing Locally

For local development, you can manually set the cookie in browser DevTools:

```javascript
// Set Bowie config
document.cookie = 'phone-config=bowie;path=/;max-age=31536000';

// Set BroPhone config
document.cookie = 'phone-config=brophone;path=/;max-age=31536000';

// Clear cookie
document.cookie = 'phone-config=;path=/;max-age=0';
```

Then refresh the page to see the configuration change.

## Troubleshooting

**Cookie not persisting:**
- Check that the domain is set correctly (`.infinitebutts.com`)
- Verify cookies are enabled in browser
- Check browser DevTools → Application → Cookies

**Redirect loop:**
- Verify redirect URLs don't point to themselves
- Check console for redirect messages
- Ensure main site (`phone.infinitebutts.com`) is not configured to redirect

**Wrong configuration loading:**
- Check cookie value in DevTools
- Clear cookies and try again
- Verify config names match cookie values exactly

## Advanced: Adding More Configurations

To add a new configuration (e.g., "sis-phone"):

1. **Add config to config-manager.js:**
```javascript
'sisphone': {
    projectName: "Sis Phone",
    theme: "dark",
    googleSheets: {
        spreadsheetId: 'SIS_SHEET_ID',
        appScriptUrl: 'SIS_SCRIPT_URL'
    }
}
```

2. **Create redirect page** (`sisphone-redirect.html`):
```javascript
setCookie('phone-config', 'sisphone');
window.location.href = 'https://phone.infinitebutts.com';
```

3. **Deploy** to `sisphone.infinitebutts.com`

## Clearing/Resetting Configuration

Users can clear their configuration by:
- Clearing browser cookies
- Visiting any redirect site to set a new configuration
- You could also create a `reset.html` that clears the cookie
