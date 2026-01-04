# Multi-Tenant Configuration Guide

This setup allows you to use different styling and back-end Google Sheets/Drive based on the URL used to access the site.

## How It Works

The system automatically detects the hostname and loads the appropriate configuration from `config-manager.js`. Each configuration can specify:

- Different Google Sheets (spreadsheet ID and Apps Script URL)
- Different Google Drive folders for audio uploads
- Different visual themes
- Different project names and settings

## Configuration File

Edit `config-manager.js` and add your hostname configurations to the `SITE_CONFIGS` object:

```javascript
const SITE_CONFIGS = {
    // Default/fallback configuration
    'default': { ... },
    
    // Add your specific hostnames
    'mysite1.com': {
        projectName: "Site 1 Phone",
        theme: "blue",
        googleSheets: {
            enabled: true,
            spreadsheetId: 'YOUR_SPREADSHEET_ID',
            appScriptUrl: 'YOUR_APPS_SCRIPT_URL'
        },
        googleDrive: {
            folderId: 'YOUR_FOLDER_ID'
        }
    },
    
    'mysite2.com': {
        projectName: "Site 2 Phone",
        theme: "dark",
        googleSheets: {
            spreadsheetId: 'DIFFERENT_SPREADSHEET_ID',
            appScriptUrl: 'DIFFERENT_APPS_SCRIPT_URL'
        }
    }
};
```

## Supported Matching Patterns

1. **Exact hostname match**: `example.com`
2. **Hostname with port**: `localhost:8000` (useful for development)
3. **Wildcard subdomain**: `*.example.com` (matches any subdomain)

The system tries matches in this order:
1. Exact match with port (`example.com:8080`)
2. Exact hostname match (`example.com`)
3. Wildcard subdomain match (`*.example.com`)
4. Falls back to `default`

## Theme System

### Available Themes

- `default` - Uses the standard styles.css without additional theme
- `blue` - Blue color scheme (uses `styles/theme-blue.css`)
- `dark` - Dark mode with purple accents (uses `styles/theme-dark.css`)

### Creating Custom Themes

1. Create a new file: `docs/styles/theme-yourname.css`
2. Override CSS variables and styles:

```css
:root {
    --primary-color: #YOUR_COLOR;
    --accent-color: #YOUR_COLOR;
    /* ... more variables */
}
```

3. Reference it in your site config:

```javascript
'yoursite.com': {
    theme: "yourname",  // Will load theme-yourname.css
    // ... rest of config
}
```

### Custom Style Variables

You can also set colors directly in the config:

```javascript
customStyles: {
    primaryColor: '#4CAF50',
    accentColor: '#2196F3'
}
```

These will override the CSS variables dynamically.

## Setting Up Different Google Sheets

For each site configuration:

1. **Create a Google Sheet** for that site
2. **Deploy the Apps Script** (use `universal-apps-script.js`)
3. **Create a Google Drive folder** for audio uploads
4. **Add the IDs to config-manager.js**:

```javascript
'yoursite.com': {
    googleSheets: {
        enabled: true,
        gid: '0',  // Usually 0 for first sheet
        spreadsheetId: 'YOUR_SPREADSHEET_ID',  // From the Sheet URL
        appScriptUrl: 'YOUR_DEPLOYED_WEB_APP_URL'  // Must end with /exec
    },
    googleDrive: {
        folderId: 'YOUR_FOLDER_ID',  // From Drive folder URL
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_FOLDER_ID'
    }
}
```

## Development Setup

For local testing with different configurations:

```javascript
'localhost:8000': {
    projectName: "Dev Site 1",
    theme: "blue",
    googleSheets: {
        spreadsheetId: 'DEV_SHEET_1'
    }
},
'localhost:8001': {
    projectName: "Dev Site 2",
    theme: "dark",
    googleSheets: {
        spreadsheetId: 'DEV_SHEET_2'
    }
}
```

Then run your local servers on different ports to test each configuration.

## Example Use Cases

### Multiple Customers
Host the same app for different customers, each with their own data:
- `customer1.yourapp.com` → Sheet 1, Blue theme
- `customer2.yourapp.com` → Sheet 2, Dark theme

### Development/Staging/Production
- `localhost:8000` → Dev sheet and theme
- `staging.yourapp.com` → Staging sheet
- `yourapp.com` → Production sheet

### Branded Instances
- `brand1.example.com` → Brand 1 colors and data
- `brand2.example.com` → Brand 2 colors and data

## Testing

1. Open your browser's developer console (F12)
2. Look for these messages:
   - `🌐 Loading configuration for: [hostname]`
   - `✅ Found configuration for: [hostname]`
   - `🎨 Applying theme: [theme]`
   - `📋 Active configuration: [config object]`

3. Verify the correct Google Sheet is loaded in the Network tab

## Troubleshooting

**Theme not loading:**
- Check that the theme CSS file exists in `docs/styles/`
- Verify the theme name matches the filename (`theme-NAME.css`)
- Check browser console for 404 errors

**Wrong Google Sheet loading:**
- Clear browser cache
- Verify hostname matches exactly (check `window.location.hostname` in console)
- Check for typos in spreadsheet IDs

**Configuration not found:**
- The system will fall back to 'default' - check console warnings
- Verify the hostname is in SITE_CONFIGS
- Remember: `www.example.com` and `example.com` are different hostnames

## Backward Compatibility

The new `config-manager.js` replaces `config.js` but maintains backward compatibility by creating a global `PHONE_CONFIG` object. Your existing app.js code will continue to work without changes.
