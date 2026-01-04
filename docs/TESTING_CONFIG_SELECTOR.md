# Testing Direct Access Config Selector

## How to Test the New Config Selector

### Test 1: Direct Access Shows Selector

1. **Clear all cookies**:
   - Open DevTools (F12)
   - Go to Application → Cookies
   - Delete all cookies for your domain
   - Also clear sessionStorage: `sessionStorage.clear()`

2. **Visit the site directly**:
   - Go to `phone.infinitebutts.com` (or `localhost:PORT`)
   - You should see a modal overlay with config selector
   - Two options: "Bowie Phone" and "BroPhone"

3. **Select a config**:
   - Click on one of the options
   - Page should reload
   - Selected config should load (check theme/title)
   - Cookie should be set

### Test 2: Cookie Persists (No Selector)

1. **With cookie already set from Test 1**:
   - Visit `phone.infinitebutts.com` directly again
   - Should NOT show selector
   - Should load your previously selected config
   - No interruption

### Test 3: Redirect Sites Skip Selector

1. **Clear cookies again**

2. **Visit redirect site**:
   - Go to `bowie-phone.infinitebutts.com`
   - Should redirect to main site
   - Should NOT show selector (came from redirect)
   - Should load Bowie config

### Test 4: Session Persistence

1. **Clear cookies only** (not sessionStorage):
   ```javascript
   document.cookie.split(";").forEach(c => {
       document.cookie = c.trim().split("=")[0] + '=;expires=Thu, 01 Jan 1970 00:00:00 UTC;path=/';
   });
   ```

2. **Refresh the page**:
   - If you selected a config earlier in the session, it should remember
   - Should NOT show selector again
   - Uses sessionStorage as backup

### Test 5: Different Referrers

**From Redirect Site** (should skip selector):
- Visit `bowie-phone.infinitebutts.com`
- Gets redirected → no selector

**From External Link** (should show selector):
- Clear cookies
- Visit from Google, bookmark, direct URL
- Should show selector

**From Same Site** (should skip selector):
- Navigate between pages on phone.infinitebutts.com
- Should not show selector again (has cookie or session)

## Expected Behavior

### When Selector SHOULD Show:
- ✅ First visit to phone.infinitebutts.com
- ✅ No cookie set
- ✅ No sessionStorage entry
- ✅ Not referred by redirect site
- ✅ Cookies cleared between visits

### When Selector Should NOT Show:
- ❌ Cookie is set (phone-config=bowie/brophone)
- ❌ SessionStorage has entry (phone-config-selected)
- ❌ Came from redirect site (bowie-phone or brophone.infinitebutts.com)
- ❌ On redirect site itself (only on main site)
- ❌ Already selected in current session

## Visual Checks

**Config Selector Modal**:
- [ ] Dark overlay (70% opacity black)
- [ ] White card in center
- [ ] Smooth fade-in animation
- [ ] Two config options with icons
- [ ] Hover effect on options (blue border, slight shift)
- [ ] Clicking option reloads page
- [ ] Selected config loads after reload

**Bowie Option**:
- 📞 Phone icon
- "Bowie Phone" title
- "Green theme • Original sequences" description

**BroPhone Option**:
- 📱 Mobile icon
- "BroPhone" title
- "Blue theme • Alternative sequences" description

## Console Messages

Check browser console for these messages:

**Direct Access**:
```
🌐 Loading configuration for: phone.infinitebutts.com
🎯 Direct access detected - will show config selector
```

**After Selection**:
```
✅ User selected configuration: bowie
🌐 Loading configuration for: phone.infinitebutts.com
🍪 Found cookie configuration: bowie
🎨 Applying theme: default
```

## Troubleshooting

**Selector doesn't appear**:
- Verify cookies are cleared
- Verify sessionStorage is cleared: `sessionStorage.clear()`
- Check you're on the main site, not a redirect site
- Check console for "Direct access detected" message

**Selector appears every time**:
- Check cookie is being set: DevTools → Application → Cookies
- Verify cookie domain is correct
- Check browser allows cookies

**Selection doesn't work**:
- Check console for errors
- Verify `configManager.selectConfig` function exists
- Try clicking with DevTools console open

**Wrong config loads**:
- Check cookie value matches selection
- Clear cache and try again
- Verify config names in SITE_CONFIGS match cookie values
