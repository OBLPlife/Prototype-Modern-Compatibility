// Appended only to the two fingerprinted original video-menu frame scripts.
// Reuse the native toggle, options container, list navigation and confirmation dialog.
if(_global.PLATFORM == _global.RAD_WIN32 && mcVideoOverlay.tglResolution != undefined &&
   mVideoOptions != undefined && !__PrototypeModernDisplayMode)
{
   var __PrototypeModernDisplayMode = true;
   var pmToggle = mcVideoOverlay.tglResolution.duplicateMovieClip("tglPrototypeModernMode",mcVideoOverlay.getNextHighestDepth());
   pmToggle.__proto__ = mcVideoOverlay.tglResolution.__proto__;
   pmToggle.mElements = new Array();
   pmToggle.mCurSelected = -1;
   pmToggle.mOnChange = null;
   pmToggle.mEnabled = true;
   pmToggle.mSupported = true;
   pmToggle.mShowArrows = false;
   pmToggle.mSymbolType = "stdtoggle";
   pmToggle.OnHighlightedUpdate = pmToggle.MouseOverHandler;
   pmToggle._visible = false;
   var pmMode = new optionscontainer(pmToggle,null,optionscontainer.DBTYPE_INT,"$PM_DISPLAY_MODE",
      [{mLabel:"$PM_DISPLAY_MODE_FULLSCREEN",mValue:0},{mLabel:"$PM_DISPLAY_MODE_BORDERLESS",mValue:1},{mLabel:"$PM_DISPLAY_MODE_WINDOWED",mValue:2}],
      undefined,undefined,true,function()
      {
         return flash.external.ExternalInterface.call("PM_GetAppliedDisplayMode");
      });
   pmMode.GetGameDBValue = function(index)
   {
      return flash.external.ExternalInterface.call("PM_GetDisplayMode");
   };
   pmMode.WriteValueToGameDB = function(forcedValue,index)
   {
      var value = forcedValue == undefined ? this.mMovie.GetSelectedValue() : forcedValue;
      return flash.external.ExternalInterface.call("PM_SetDisplayMode",value);
   };
   pmMode.RestoreGameDBDefault = function()
   {
      flash.external.ExternalInterface.call("PM_SetDisplayMode",0);
   };
   // Use the live PC text-bible query. Our four English resources are supplied
   // by its native query provider, not by control-side string replacement.
   pmMode.__pmQueryLabel = function(key)
   {
      var text = flash.external.ExternalInterface.call("FormatText",key);
      flash.external.ExternalInterface.call("PM_ReportAction","display-mode-text",key,text);
      return text;
   };
   pmMode.__pmInitialize = pmMode.Initialize;
   pmMode.Initialize = function(maintainInitialValues)
   {
      // Resolve when the native control is initialized, including re-entry.
      // Only labels change; mode values and the confirmation snapshot stay native.
      this.mLabel = this.__pmQueryLabel("$PM_DISPLAY_MODE");
      this.mData[0].mLabel = this.__pmQueryLabel("$PM_DISPLAY_MODE_FULLSCREEN");
      this.mData[1].mLabel = this.__pmQueryLabel("$PM_DISPLAY_MODE_BORDERLESS");
      this.mData[2].mLabel = this.__pmQueryLabel("$PM_DISPLAY_MODE_WINDOWED");
      this.mMovie.mElements = new Array();
      this.mMovie.mCurSelected = -1;
      this.__pmInitialize(maintainInitialValues);
   };
   var pmIndex = 0;
   while(pmIndex < mVideoOptions.length && mVideoOptions[pmIndex].mDBName != "SettingVideoResolution") pmIndex++;
   mVideoOptions.splice(pmIndex,0,pmMode);
   // mOptions references the same array; Initialize02 lays out/navigates all rows.
   var __pmOriginalSaveSettings = SaveSettings;
   SaveSettings = function()
   {
      flash.external.ExternalInterface.call("PM_CommitDisplayMode");
      __pmOriginalSaveSettings.apply(this,arguments);
   };
}
