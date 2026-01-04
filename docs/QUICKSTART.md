# Cookie-Based Multi-Tenant Phone System - Quick Start

## 🎯 What You Have Now

A phone sequence management system that can run multiple independent instances from one codebase, with users selecting their instance via URL.

## 🌐 The Three Sites

| Site | Purpose | What It Does |
|------|---------|--------------|
| **phone.infinitebutts.com** | Main app | Reads cookie, loads appropriate config & data |
| **bowie-phone.infinitebutts.com** | Bowie selector | Sets `phone-config=bowie` cookie → redirects |
| **brophone.infinitebutts.com** | BroPhone selector | Sets `phone-config=brophone` cookie → redirects |

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

### 2. Update config-manager.js

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

| Setting | Bowie Default | BroPhone Example |
|---------|---------------|------------------|
| **Theme** | `default` (green) | `blue` |
| **Google Sheet** | Your current sheet | New BroPhone sheet |
| **Drive Folder** | Your current folder | New BroPhone folder |
| **Project Name** | "Bowie Phone Sequences" | "BroPhone Sequences" |
| **Colors** | Green/Blue | Blue/Dark Blue |

## 🎨 Available Themes

- `default` - Green accent colors (Bowie)
- `blue` - Blue color scheme (BroPhone)
- `dark` - Dark mode with purple accents

Create custom themes by adding `styles/theme-YOURNAME.css`

## 🔧 Admin Tools

**Config Switcher**: Deploy `config-switcher.html` for a UI to switch configs without visiting redirect sites.

Access at: `https://phone.infinitebutts.com/config-switcher.html`

## 📝 Files Created

| File | Purpose |
|------|---------|
| `config-manager.js` | Core multi-tenant system |
| `bowie-redirect.html` | Bowie Phone selector page |
| `brophone-redirect.html` | BroPhone selector page |
| `config-switcher.html` | Admin config switcher |
| `theme-blue.css` | Blue theme for BroPhone |
| `theme-dark.css` | Dark theme option |
| `COOKIE_CONFIG_SETUP.md` | Detailed technical docs |
| `DEPLOYMENT.md` | Full deployment guide |
| `MULTI_TENANT_CONFIG.md` | Configuration reference |

## 🔍 Troubleshooting

**See wrong data?**
- Check cookie in DevTools → Application → Cookies
- Verify spreadsheet IDs are different in config-manager.js

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
2. ✅ Update the placeholder IDs in config-manager.js
3. ✅ Deploy to your three subdomains
4. ✅ Test both configurations
5. ✅ Share the URLs with users

## 💡 Tips

- Cookie lasts 365 days - users won't need to re-select often
- Each config can have completely separate data
- Add more configs anytime by following the same pattern
- Use config-switcher.html for easy testing during development

---

**Ready?** Update the BroPhone IDs in config-manager.js and deploy! 🚀
