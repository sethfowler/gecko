// -*- Mode: js2; tab-width: 2; indent-tabs-mode: nil; js2-basic-offset: 2; js2-skip-preprocessor-directives: t; -*-
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Mobile Browser.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Mark Finkle <mfinkle@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

Components.utils.import("resource://gre/modules/utils.js");

const TOOLBARSTATE_LOADING        = 1;
const TOOLBARSTATE_LOADED         = 2;

const UIMODE_NONE              = 0;
const UIMODE_URLVIEW           = 1;
const UIMODE_URLEDIT           = 2;
const UIMODE_BOOKMARK          = 3;
const UIMODE_TABS              = 4;
const UIMODE_CONTROLS          = 5;
const UIMODE_PANEL             = 6;

const kMaxEngines = 4;
const kDefaultFavIconURL = "chrome://browser/skin/images/default-favicon.png";

[
  ["gContentBox",            "content"],
].forEach(function (elementGlobal) {
  var [name, id] = elementGlobal;
  window.__defineGetter__(name, function () {
    var element = document.getElementById(id);
    if (!element)
      return null;
    delete window[name];
    return window[name] = element;
  });
  window.__defineSetter__(name, function (val) {
    delete window[name];
    return window[name] = val;
  });
});


var BrowserUI = {
  _panel : null,
  _edit : null,
  _throbber : null,
  _autocompleteNavbuttons : null,
  _favicon : null,
  _faviconLink : null,
  _editingBookmark : null,

  _titleChanged : function(aDocument) {
    var browser = Browser.selectedBrowser;
    if (browser && aDocument != browser.contentDocument)
      return;

    var caption = aDocument.title;
    if (!caption) {
      caption = this.getDisplayURI(browser);
      if (caption == "about:blank")
        caption = "";
    }
    this._edit.value = caption;

    var docElem = document.documentElement;
    var title = "";
    if (aDocument.title)
      title = aDocument.title + docElem.getAttribute("titleseparator");
    document.title = title + docElem.getAttribute("titlemodifier");
  },

  _linkAdded : function(aEvent) {
    var link = aEvent.originalTarget;
    if (!link || !link.href)
      return;

    if (/\bicon\b/i(link.rel)) {
      this._faviconLink = link.href;

      // If the link changes after pageloading, update it right away.
      // otherwise we wait until the pageload finishes
      if (this._favicon.src != "")
        this._setIcon(this._faviconLink);
    }
  },

  _tabSelect : function(aEvent) {
    var browser = Browser.selectedBrowser;
    this._titleChanged(browser.contentDocument);
    this._favicon.src = browser.mIconURL || kDefaultFavIconURL;

    // for new tabs, _tabSelect & update(TOOLBARSTATE_LOADED) are called when
    // about:blank is loaded. set _faviconLink here so it is not overriden in update
    this._faviconLink = this._favicon.src;
    this.updateIcon();
  },

  _setIcon : function(aURI) {
    var ios = Cc["@mozilla.org/network/io-service;1"].getService(Ci.nsIIOService);
    try {
      var faviconURI = ios.newURI(aURI, null, null);
    }
    catch (e) {
      faviconURI = null;
    }

    var fis = Cc["@mozilla.org/browser/favicon-service;1"].getService(Ci.nsIFaviconService);
    if (!faviconURI || faviconURI.schemeIs("javascript") || fis.isFailedFavicon(faviconURI))
      faviconURI = ios.newURI(kDefaultFavIconURL, null, null);

    var browser = getBrowser();
    browser.mIconURL = faviconURI.spec;

    fis.setAndLoadFaviconForPage(browser.currentURI, faviconURI, true);
    this._favicon.src = faviconURI.spec;
  },

  _getHistory : function(aCount) {
    var items = [];

    var hs = Cc["@mozilla.org/browser/nav-history-service;1"].getService(Ci.nsINavHistoryService);
    var options = hs.getNewQueryOptions();
    options.queryType = options.QUERY_TYPE_HISTORY;
    //options.sortingMode = options.SORT_BY_VISITCOUNT_DESCENDING;
    options.maxResults = aCount;
    //options.resultType = Ci.nsINavHistoryQueryOptions.RESULTS_AS_URI;
    var query = hs.getNewQuery();
    var result = hs.executeQuery(query, options);
    var rootNode = result.root;
    rootNode.containerOpen = true;
    var cc = rootNode.childCount;
    for (var i=0; i<cc; ++i) {
      var node = rootNode.getChild(i);
      items.push(node);
    }
    rootNode.containerOpen = false;

    return items;
  },

  _showToolbar : function(aShow) {
    if (aShow) {
      ws.freeze("toolbar-main");
      ws.moveFrozenTo("toolbar-main", 0, 0);
    }
    else {
      ws.unfreeze("toolbar-main");
    }
  },

  _editToolbar : function(aEdit) {
    var icons = document.getElementById("urlbar-icons");
    if (aEdit) {
      icons.setAttribute("mode", "edit");
      this._edit.readOnly = false;

      let urlString = this.getDisplayURI(Browser.selectedBrowser);
      if (urlString == "about:blank")
        urlString = "";
      this._edit.value = urlString;

      this._edit.inputField.focus();
      this._edit.select();
    }
    else {
      icons.setAttribute("mode", "view");
      this._edit.readOnly = true;
      this._edit.inputField.blur();
      this._edit.reallyClosePopup();
    }
  },

  _initPanel : function() {
    let addons = document.getElementById("addons-container");
    if (!addons.hasAttribute("src"))
      addons.setAttribute("src", "chrome://mozapps/content/extensions/extensions.xul");
    let dloads = document.getElementById("downloads-container");
    if (!dloads.hasAttribute("src"))
      dloads.setAttribute("src", "chrome://mozapps/content/downloads/downloads.xul");
  },

  switchPane : function(id) {
    document.getElementById("panel-items").selectedPanel = document.getElementById(id);
  },

  sizeControls : function() {
    let windowW = window.innerWidth;
    let windowH = window.innerHeight;

    let toolbar = document.getElementById("toolbar-main");
    if (!this._toolbarH)
      this._toolbarH = toolbar.boxObject.height;

    toolbar.width = windowW;

    let popup = document.getElementById("popup_autocomplete");
    popup.height = windowH - this._toolbarH;
    popup.width = windowW;

    // notification box
    document.getElementById("notifications").width = windowW;

    // sidebars
    let sideBarHeight = windowH - this._toolbarH;
    document.getElementById("browser-controls").height = sideBarHeight;
    document.getElementById("tabs-container").height = sideBarHeight;
  },

  init : function() {
    this._edit = document.getElementById("urlbar-edit");
    this._edit.addEventListener("click", this, false);
    this._edit.addEventListener("blur", this, false);
    this._edit.addEventListener("keypress", this, true);
    this._throbber = document.getElementById("urlbar-throbber");
    this._favicon = document.getElementById("urlbar-favicon");
    this._favicon.addEventListener("error", this, false);
    this._autocompleteNavbuttons = document.getElementById("autocomplete_navbuttons");

    // XXX these really want to listen whatever is the current browser, not any browser
    let browsers = document.getElementById("browsers");
    browsers.addEventListener("DOMTitleChanged", this, true);
    browsers.addEventListener("DOMLinkAdded", this, true);

    document.getElementById("tabs").addEventListener("TabSelect", this, true);
  },

  update : function(aState) {
    var icons = document.getElementById("urlbar-icons");

    switch (aState) {
      case TOOLBARSTATE_LOADED:
        icons.setAttribute("mode", "view");

        if (!this._faviconLink) {
          this._faviconLink = Browser.selectedBrowser.currentURI.prePath + "/favicon.ico";
        }
        this._setIcon(this._faviconLink);
        this.updateIcon();
        this._faviconLink = null;
        break;

      case TOOLBARSTATE_LOADING:
        this.show(UIMODE_URLVIEW);
        icons.setAttribute("mode", "loading");

        ws.panTo(0, -60, true);

        this._favicon.src = "";
        this._faviconLink = null;
        this.updateIcon();
        break;
    }
  },

  updateIcon : function() {
    if (Browser.selectedTab.isLoading()) {
      this._throbber.hidden = false;
      this._throbber.setAttribute("loading", "true");
      this._favicon.hidden = true;
    }
    else {
      this._favicon.hidden = false;
      this._throbber.hidden = true;
      this._throbber.removeAttribute("loading");
    }
  },

  getDisplayURI : function(browser) {
    var uri = browser.currentURI;

    if (!this._URIFixup)
      this._URIFixup = Cc["@mozilla.org/docshell/urifixup;1"].getService(Ci.nsIURIFixup);

    try {
      uri = this._URIFixup.createExposableURI(uri);
    } catch (ex) {}

    return uri.spec;
  },

  /* Set the location to the current content */
  setURI : function() {
    var browser = Browser.selectedBrowser;

    // FIXME: deckbrowser should not fire TabSelect on the initial tab (bug 454028)
    if (!browser.currentURI)
      return;

    var back = document.getElementById("cmd_back");
    var forward = document.getElementById("cmd_forward");

    back.setAttribute("disabled", !browser.canGoBack);
    forward.setAttribute("disabled", !browser.canGoForward);

    // Check for a bookmarked page
    this.updateStar();

    var urlString = this.getDisplayURI(browser);
    if (urlString == "about:blank") {
      urlString = "";
      this.show(UIMODE_URLEDIT);
    }

    this._edit.value = urlString;
  },

  goToURI : function(aURI) {
    this._edit.reallyClosePopup();

    if (!aURI)
      aURI = this._edit.value;

    var flags = Ci.nsIWebNavigation.LOAD_FLAGS_ALLOW_THIRD_PARTY_FIXUP;
    getBrowser().loadURIWithFlags(aURI, flags, null, null);
    this.show(UIMODE_URLVIEW);
  },

  search : function() {
    var queryURI = "http://www.google.com/search?q=" + this._edit.value + "&hl=en&lr=&btnG=Search";
    getBrowser().loadURI(queryURI, null, null, false);

    this.show(UIMODE_URLVIEW);
  },

  showAutoComplete : function(showDefault) {
    this.updateSearchEngines();
    this._edit.showHistoryPopup();
  },

  doButtonSearch : function(button) {
    if (!("engine" in button) || !button.engine)
      return;

    var urlbar = this._edit;
    urlbar.open = false;
    var value = urlbar.value;

    var submission = button.engine.getSubmission(value, null);
    getBrowser().loadURI(submission.uri.spec, null, submission.postData, false);
  },

  engines : null,
  updateSearchEngines : function () {
    if (this.engines)
      return;

    var searchService = Cc["@mozilla.org/browser/search-service;1"].getService(Ci.nsIBrowserSearchService);
    var engines = searchService.getVisibleEngines({ });
    this.engines = engines;

    const kXULNS = "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";
    var container = this._autocompleteNavbuttons;
    for (var e = 0; e < kMaxEngines && e < engines.length; e++) {
      var button = document.createElementNS(kXULNS, "toolbarbutton");
      var engine = engines[e];
      button.id = engine.name;
      button.setAttribute("label", engine.name);
      button.className = "searchengine";
      if (engine.iconURI)
        button.setAttribute("image", engine.iconURI.spec);
      container.appendChild(button);
      button.engine = engine;
    }
  },

  mode : UIMODE_NONE,
  show : function(aMode) {
    if (this.mode == aMode)
      return;

    this.mode = aMode;

    let bookmark = document.getElementById("bookmark-container");
    let container = document.getElementById("browser-container");
    let panelUI = document.getElementById("panel-container");

    if (aMode == UIMODE_URLVIEW) {
      this._showToolbar(false);
      this._editToolbar(false);

      bookmark.hidden = true;
      panelUI.hidden = true;
    }
    else if (aMode == UIMODE_URLEDIT) {
      this._showToolbar(true);
      this._editToolbar(true);

      bookmark.hidden = true;
      panelUI.hidden = true;
    }
    else if (aMode == UIMODE_BOOKMARK) {
      this._showToolbar(true);
      this._editToolbar(false);

      panelUI.hidden = true;
      bookmark.hidden = false;
      bookmark.width = container.boxObject.width;
    }
    else if (aMode == UIMODE_PANEL) {
      bookmark.hidden = true;
      panelUI.hidden = false;
      panelUI.width = container.boxObject.width;
      panelUI.height = container.boxObject.height;
      this._initPanel();
    }
    else if (aMode == UIMODE_TABS || aMode == UIMODE_CONTROLS) {
      panelUI.hidden = true;
    }
    else if (aMode == UIMODE_NONE) {
      this._showToolbar(false);
      this._edit.reallyClosePopup();
      bookmark.hidden = true;
      panelUI.hidden = true;
    }
  },

  updateStar : function() {
    var star = document.getElementById("tool-star");
    if (PlacesUtils.getMostRecentBookmarkForURI(Browser.selectedBrowser.currentURI) != -1)
      star.setAttribute("starred", "true");
    else
      star.removeAttribute("starred");
  },

  goToBookmark : function(aEvent) {
    if (aEvent.originalTarget.localName == "button")
      return;

    var list = document.getElementById("urllist-items")
    BrowserUI.goToURI(list.selectedItem.value)
  },

  showHistory : function() {
    // XXX Fix me with a real UI
  },

  showBookmarks : function () {
    BookmarkList.show();
  },

  newTab : function() {
    ws.panTo(0, -60, true);
    Browser.addTab("about:blank", true);
    this.show(UIMODE_URLEDIT);
  },

  closeTab : function(aTab) {
    Browser.closeTab(aTab);
  },

  selectTab : function(aTab) {
    Browser.selectedTab = aTab;
  },

  hideTabs : function() {
    ws.panTo(0, 0);
    this.show(UIMODE_NONE);
  },

  handleEvent: function (aEvent) {
    switch (aEvent.type) {
      // Browser events
      case "DOMTitleChanged":
        this._titleChanged(aEvent.target);
        break;
      case "DOMLinkAdded":
        this._linkAdded(aEvent);
        break;
      case "TabSelect":
        this._tabSelect(aEvent);
        break;
      // URL textbox events
      case "click":
        this.doCommand("cmd_openLocation");
        break;
      case "keypress":
        if (aEvent.keyCode == aEvent.DOM_VK_ESCAPE) {
          this._edit.reallyClosePopup();
          this.show(UIMODE_URLVIEW);
        }
        break;
      // Favicon events
      case "error":
        this._favicon.src = "chrome://browser/skin/images/default-favicon.png";
        break;
    }
  },

  supportsCommand : function(cmd) {
    var isSupported = false;
    switch (cmd) {
      case "cmd_back":
      case "cmd_forward":
      case "cmd_reload":
      case "cmd_stop":
      case "cmd_search":
      case "cmd_go":
      case "cmd_openLocation":
      case "cmd_star":
      case "cmd_bookmarks":
      case "cmd_menu":
      case "cmd_newTab":
      case "cmd_closeTab":
      case "cmd_actions":
      case "cmd_panel":
      case "cmd_sanitize":
      case "cmd_zoomin":
      case "cmd_zoomout":
        isSupported = true;
        break;
      default:
        isSupported = false;
        break;
    }
    return isSupported;
  },

  isCommandEnabled : function(cmd) {
    return true;
  },

  doCommand : function(cmd) {
    var browser = getBrowser();
    switch (cmd) {
      case "cmd_back":
        browser.goBack();
        break;
      case "cmd_forward":
        browser.goForward();
        break;
      case "cmd_reload":
        browser.reload();
        break;
      case "cmd_stop":
        browser.stop();
        break;
      case "cmd_search":
        this.search();
        break;
      case "cmd_go":
        this.goToURI();
        break;
      case "cmd_openLocation":
        this.show(UIMODE_URLEDIT);
        setTimeout(function () { BrowserUI.showAutoComplete(); }, 0);
        break;
      case "cmd_star":
      {
        var bookmarkURI = browser.currentURI;
        var bookmarkTitle = browser.contentDocument.title;

        if (PlacesUtils.getMostRecentBookmarkForURI(bookmarkURI) == -1) {
          var bookmarkId = PlacesUtils.bookmarks.insertBookmark(PlacesUtils.bookmarks.bookmarksMenuFolder, bookmarkURI, PlacesUtils.bookmarks.DEFAULT_INDEX, bookmarkTitle);
          BrowserUI.updateStar();

          var ios = Cc["@mozilla.org/network/io-service;1"].getService(Ci.nsIIOService);
          var favicon = document.getElementById("urlbar-favicon");
          var faviconURI = ios.newURI(favicon.src, null, null);

          PlacesUtils.favicons.setAndLoadFaviconForPage(bookmarkURI, faviconURI, true);

          this.show(UIMODE_NONE);
        }
        else {
          this.show(UIMODE_BOOKMARK);
          BookmarkHelper.edit(bookmarkURI);
        }
        break;
      }
      case "cmd_bookmarks":
        this.showBookmarks();
        break;
      case "cmd_menu":
        break;
      case "cmd_newTab":
        this.newTab();
        break;
      case "cmd_closeTab":
        this.closeTab();
        break;
      case "cmd_sanitize":
        Sanitizer.sanitize();
        break;
      case "cmd_panel":
      {
        var mode = (this.mode != UIMODE_PANEL ? UIMODE_PANEL : UIMODE_CONTROLS);
        this.show(mode);
        break;
      }
      case "cmd_zoomin":
        Browser.canvasBrowser.zoom(-1);
        break;
      case "cmd_zoomout":
        Browser.canvasBrowser.zoom(1);
        break;
    }
  }
};

var BookmarkHelper = {
  edit: function(aURI) {
    var bookmarkEdit = document.getElementById("bookmark-edit");
    bookmarkEdit.startEditing(aURI);
    window.addEventListener("keypress", this, true);
  },

  close: function() {
    var bookmarkEdit = document.getElementById("bookmark-edit");
    window.removeEventListener("keypress", this, true);
    BrowserUI.show(UIMODE_NONE);
    BrowserUI.updateStar();
  },

  handleEvent: function(aEvent) {
    if (aEvent.keyCode == aEvent.DOM_VK_ESCAPE)
      document.getElementById("bookmark-edit").stopEditing(true);
  }
};

var BookmarkList = {
  _panel: null,
  _bookmarks: null,

  show: function() {
    let container = document.getElementById("browser-container");
    this._panel = document.getElementById("bookmarklist-container");
    this._panel.hidden = false;
    this._panel.width = container.boxObject.width;
    this._panel.height = container.boxObject.height;

    this._bookmarks = document.getElementById("bookmark-items");
    this._bookmarks.manageUI = false;
    this._bookmarks.openFolder();

    window.addEventListener("keypress", this, true);
  },

  close: function() {
    window.removeEventListener("keypress", this, true);
    BrowserUI.updateStar();

    if (this._bookmarks.isEditing)
      this._bookmarks.stopEditing();
    this._bookmarks.blur();

    this._panel.hidden = true;
  },

  toggleManage: function() {
    this._bookmarks.manageUI = !(this._bookmarks.manageUI);
  },

  openBookmark: function() {
    let item = this._bookmarks.activeItem;
    if (item.uri) {
      this._panel.hidden = true;
      BrowserUI.goToURI(item.uri)
    }
  },

  handleEvent: function(aEvent) {
    if (aEvent.keyCode == aEvent.DOM_VK_ESCAPE)
      this.close();
  }
};

var FolderHelper = {
  _control: null,
  _panel: null,

  show: function(aControl) {
    let container = document.getElementById("browser-container");
    this._panel = document.getElementById("folder-container");
    this._panel.hidden = false;
    this._panel.width = container.boxObject.width;
    this._panel.height = container.boxObject.height;

    this._control = aControl;

    let folders = document.getElementById("folder-items");
    folders.openFolder();
  },

  close: function() {
    this._panel.hidden = true;
  },

  selectFolder: function() {
    let folders = document.getElementById("folder-items");
    let folderId = PlacesUtils.bookmarks.getFolderIdForItem(this._control.activeItem.itemId);
    if (folders.selectedItem.itemId != folderId) {
      PlacesUtils.bookmarks.moveItem(this._control.activeItem.itemId, folders.selectedItem.itemId, PlacesUtils.bookmarks.DEFAULT_INDEX);
      this._control.removeItem(this._control.activeItem);
    }
    this.close();
  }
};
