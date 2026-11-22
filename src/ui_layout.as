// Original layout extension. Original timelines, map scale and hit handlers remain native.
// Depths below were independently read from this installation's original GFX resources.
function PM_Place(mc, dx, scale, center)
{
   if(mc == undefined) return;
   var p = mc.__pmLayout;
   if(p == undefined) p = {x:mc._x, sx:mc._xscale};
   // A timeline can supply a new transform between frames. Never accumulate our delta.
   if(p.lastX != undefined && Math.abs(mc._x - p.lastX) > 0.02) p.x = mc._x;
   if(p.lastSX != undefined && Math.abs(mc._xscale - p.lastSX) > 0.02) p.sx = mc._xscale;
   mc._x = center + (p.x - center) * scale + dx;
   mc._xscale = p.sx * scale;
   p.lastX = mc._x;
   p.lastSX = mc._xscale;
   mc.__pmLayout = p;
}
function PM_Depths(parent, base, depths, dx, scale, center)
{
   var i = 0;
   while(i < depths.length)
   {
      PM_Place(parent.getInstanceAtDepth(base + depths[i]),dx,scale,center);
      i++;
   }
}
function PM_Frame(frame, left, right, ratio)
{
   if(frame == undefined || frame.mcPulsingBg == undefined || frame.mcImageAnchor == undefined) return;
   var base = frame.mcPulsingBg.getDepth() - 1;
   // Depth 8 is the original Gentek logo; keep its native aspect ratio.
   PM_Depths(frame,base,[1,4,6,9,19],0,ratio,640);
   PM_Depths(frame,base,[12,18],left,1,0);
   PM_Depths(frame,base,[17],right,1,0);
}
function PM_PauseOverlay(overlay, left, right, ratio)
{
   if(overlay == undefined || overlay.mcBk0 == undefined || overlay.mcBtn8 == undefined) return;
   var base = overlay.mcBk0.getDepth() - 7;
   if(overlay.__pmPanelLeft == undefined) overlay.__pmPanelLeft = overlay.mcBk0.getBounds(_root).xMin;
   var panelLeft = ratio == 1 ? 0 : left - overlay.__pmPanelLeft;
   var localLeft = panelLeft * 100 / overlay._xscale;
   var localRight = right * 100 / overlay._xscale;
   var center = (640 - overlay._x) * 100 / overlay._xscale;
   // Menu buttons, their backgrounds, hints and left panel decoration move together.
   PM_Depths(overlay,base,[1,3,5,7,9,11,13,15,17,19,21,23,25,34,43,52,61,70,79,88,97,
      106,108,110,112,114,116,124,126,132,138,144,220,276],localLeft,1,0);
   PM_Depths(overlay,base,[122,248,304],localRight,1,0);
   PM_Depths(overlay,base,[118,120,150,196,208],0,ratio,center);
}
function PM_Map(screen, left, right, ratio)
{
   if(screen.mcPauseMap == undefined) return;
   var base = screen.mcPauseMap.getDepth() - 73;
   // Extend decorative background and move the original occluding side panels.
   // mcPauseMap, mcMap, map markers and the world-coordinate conversions are untouched.
   PM_Depths(screen,base,[1],0,ratio,640);
   PM_PauseLogo(screen,base,ratio);
   PM_Depths(screen,base,[13,15,17,19,21,23,25,77,79],right,1,0);
   PM_Depths(screen,base,[27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71],left,1,0);
   if(screen.mcLegend != undefined)
   {
      if(screen.__pmLegendRight == undefined) screen.__pmLegendRight = screen.mcLegend.getBounds(_root).xMax;
      PM_Place(screen.mcLegend,ratio == 1 ? 0 : right + 1280 - screen.__pmLegendRight,1,0);
   }
   PM_Place(screen.mcInfo,left,1,0);
   // Expand mouse map/scroll hit regions with the visible map, using their original widths.
   var names = ["autoscrollMapBounds","autoscrollTop","autoscrollBottom"];
   var i = 0;
   while(i < names.length)
   {
      var mc = screen[names[i]];
      if(mc != undefined)
      {
         if(mc.__pmWidth == undefined) mc.__pmWidth = mc._width;
         PM_Place(mc,left,1 + (right - left) / mc.__pmWidth,mc.__pmLayout == undefined ? mc._x : mc.__pmLayout.x);
      }
      i++;
   }
}
function PM_FadeText(text, fade, ratio, center, shift)
{
   if(text == undefined || text._parent == undefined) return;
   // Call only for verified static placements, never an animated/replaced owner.
   if(center == undefined) center = 0;
   if(shift == undefined) shift = 0;
   var zero = {x:0,y:0};
   var unit = {x:1,y:0};
   text._parent.localToGlobal(zero);
   text._parent.localToGlobal(unit);
   fade.globalToLocal(zero);
   fade.globalToLocal(unit);
   var sx = unit.x - zero.x;
   if(Math.abs(sx) < 0.000001) return;
   // Undo the canvas scale for both the glyph width and its original position.
   PM_Place(text,(zero.x - center) * (1 / ratio - 1) / sx + shift / (ratio * sx),1 / ratio,0);
}
function PM_PauseLogo(screen, base, ratio)
{
   // These verified pause backgrounds share a one-frame Gentek sprite at
   // depth 5. Undo only its parent's canvas expansion, keeping the original
   // symbol size and position while the surrounding background stays wide.
   var background = screen.getInstanceAtDepth(base + 1);
   if(background == undefined || background.mcPulsingBg == undefined) return;
   var backgroundBase = background.mcPulsingBg.getDepth() - 1;
   var logo = background.getInstanceAtDepth(backgroundBase + 5);
   if(logo == undefined || logo.__pmPauseLogoRatio == ratio) return;
   PM_FadeText(logo,_root,ratio,640);
   logo.__pmPauseLogoRatio = ratio;
}
function PM_BlackFade(fade, ratio)
{
   if(fade == undefined || fade.mcBlack == undefined) return;
   // This canvas is placed once in the one-frame main root. It has no parent
   // timeline moves. Scale it as a unit, leaving every animated layer inside it
   // untouched. No adjoining extra rectangles and therefore no internal seams.
   if(fade.__pmCanvasSX == undefined) fade.__pmCanvasSX = fade._xscale;
   fade._xscale = fade.__pmCanvasSX * ratio;
   if(fade.__pmReportedRatio != ratio)
   {
      flash.external.ExternalInterface.call("PM_ReportUiCanvas",0,ratio,fade._xscale);
      fade.__pmReportedRatio = ratio;
   }
   var base = fade.mcBlack.getDepth() - 3;
   if(fade.mcPulse != undefined)
   {
      PM_LoadingSymbol(fade.mcPulse,base,ratio);
      var loading = fade.mcPulse.getInstanceAtDepth(base + 10);
      if(loading != undefined) PM_FadeText(loading.txtLabel,fade,ratio);
   }
   if(fade.typewriter != undefined)
   {
      if(fade.typewriter.mcTextWrapper != undefined) PM_FadeText(fade.typewriter.mcTextWrapper.typeTxt,fade,ratio);
      if(fade.typewriter.mcShadowText != undefined) PM_FadeText(fade.typewriter.mcShadowText.typeTxt,fade,ratio);
   }
}
function PM_LoadingSymbol(pulse, base, ratio)
{
   // Sprite 11 is the original masked hexagon sheet: one static frame, no
   // scripts. Its placement at depth 1 has animated matrix and color values.
   // Keep that native owner receiving its timeline. Render a copy of the same
   // symbol at unused depth 2, before every later original background/text.
   // _visible changes only visibility, not the native transform-update flag.
   var original = pulse.getInstanceAtDepth(base + 1);
   var copy = pulse.getInstanceAtDepth(base + 2);
   if(copy != undefined && copy != pulse.__PrototypeModernLoadingSymbol) return;
   if(original == undefined || ratio == 1)
   {
      if(copy != undefined) copy._visible = false;
      if(original != undefined && pulse.__pmHexOwner == original)
         original._visible = pulse.__pmHexVisible;
      return;
   }
   if(copy == undefined)
      copy = original.duplicateMovieClip("__PrototypeModernLoadingSymbol",base + 2);
   if(copy == undefined) return;
   if(pulse.__pmHexOwner != original)
   {
      pulse.__pmHexOwner = original;
      pulse.__pmHexVisible = original._visible;
   }
   // mcPulse is statically placed at (0,0). Cancel the canvas scale only for
   // this symbol, preserving its original world position, size and mask.
   copy._x = original._x / ratio;
   copy._xscale = original._xscale / ratio;
   copy._y = original._y;
   copy._yscale = original._yscale;
   copy._rotation = original._rotation;
   var sourceColor = new Color(original);
   var copyColor = new Color(copy);
   copyColor.setTransform(sourceColor.getTransform());
   copy._visible = pulse.__pmHexVisible;
   original._visible = false;
   if(pulse.__pmHexRatio != ratio)
   {
      flash.external.ExternalInterface.call("PM_ReportUiCanvas",26,ratio,copy._xscale);
      pulse.__pmHexRatio = ratio;
   }
}
function PM_RootShift(mc, dx)
{
   if(mc == undefined || mc._parent == undefined) return;
   var a = {x:0,y:0};
   var b = {x:dx,y:0};
   _root.localToGlobal(a);
   _root.localToGlobal(b);
   mc._parent.globalToLocal(a);
   mc._parent.globalToLocal(b);
   PM_Place(mc,b.x - a.x,1,0);
}
function PM_StaticCanvas(parent, base, depths, ratio)
{
   if(parent == undefined) return;
   var center = {x:640,y:360};
   _root.localToGlobal(center);
   parent.globalToLocal(center);
   PM_Depths(parent,base,depths,0,ratio,center.x);
}
function PM_FrontFrame(overlay, left, ratio)
{
   if(overlay == undefined || overlay.mcScreenName == undefined) return;
   var base = overlay.mcScreenName.getDepth() - 41;
   PM_StaticCanvas(overlay.getInstanceAtDepth(base + 1),base,[1,3,5,7,9,11,13,15],ratio);
   PM_StaticCanvas(overlay.getInstanceAtDepth(base + 17),base,[1],ratio);
   PM_RootShift(overlay.mcScreenName.txtBox,left);
}
function PM_FrontCanvas(screen, left, ratio)
{
   var name = screen.mScreenName;
   var enabled = name == "protolegalscreen" || name == "protomainmenu";
   // mcAnchor is placed once in the one-frame main root. Its children have
   // animated matrices, colors and replacements; leave all of those native.
   // Keep the restore record on _root: loadMovie clears screen-local variables
   // and may either retain or recreate the loaded clip's transform.
   var state = _root.__pmFrontCanvas;
   if(!enabled || ratio == 1)
   {
      if(state != undefined)
      {
         screen._x = state.x;
         screen._xscale = state.sx;
         if(enabled) PM_FrontContent(screen,left,1,ratio);
         flash.external.ExternalInterface.call("PM_ReportUiCanvas",4,1,screen._xscale);
         delete _root.__pmFrontCanvas;
      }
      return;
   }
   if(state == undefined)
   {
      state = {x:screen._x,sx:screen._xscale};
      _root.__pmFrontCanvas = state;
   }
   // Original scan placements are offset by 0.15 units. One unit of overscan
   // on each edge puts their continuous outer boundary beyond the viewport.
   var canvasRatio = ratio + 2 / 1280;
   screen._x = 640 + (state.x - 640) * canvasRatio;
   screen._xscale = state.sx * canvasRatio;
   PM_FrontContent(screen,left,canvasRatio,ratio);
   if(state.name != name || state.ratio != canvasRatio)
   {
      flash.external.ExternalInterface.call("PM_ReportUiCanvas",name == "protolegalscreen" ? 2 : 1,canvasRatio,screen._xscale);
      state.name = name;
      state.ratio = canvasRatio;
   }
}
function PM_FrontContent(screen, left, canvasRatio, ratio)
{
   if(screen.mScreenName == "protolegalscreen")
   {
      // The typewriter text field is static inside one-frame Sprite 26.
      if(screen.mcText != undefined) PM_FadeText(screen.mcText.mLegalText,screen,canvasRatio,640,0);
      return;
   }
   // protoButton changes mcText on highlight/disable. Its txtLabel is static
   // inside each one-frame text sprite, so parent transitions remain native.
   var names = ["mcBtnNew","mcBtnContinue","mcBtnOptions","mcBtnExtra","mcBtnQuit","mcBtnDev"];
   var i = 0;
   while(i < names.length)
   {
      var button = screen[names[i]];
      if(button != undefined && button.mcText != undefined)
         PM_FadeText(button.mcText.txtLabel,screen,canvasRatio,640,left);
      i++;
   }
   // The overlay's root placement is static. Undo the canvas transform there
   // so its dynamic navigation hint fields keep their native highlight states.
   PM_FadeText(screen.mcOverlay,screen,canvasRatio,640,0);
   PM_FrontFrame(screen.mcOverlay,left,ratio);
}
function PM_SaveLoad(screen, left, right, ratio)
{
   if(screen.mScreenName != "protosaveload" || screen.mcStorageBackdrop == undefined) return;
   var base = screen.mcStorageBackdrop.getDepth() - 1;
   // Root placements are static in all three frames. Keep the central native
   // save dialog and its thumbnails/scroll handling at their original size.
   PM_StaticCanvas(screen,base,[3,5],ratio);
   var depths = [9,11,13,15,17,19];
   var i = 0;
   while(i < depths.length)
   {
      PM_RootShift(screen.getInstanceAtDepth(base + depths[i]),left);
      i++;
   }
   // StorageBackdrop's child fades for 25 frames. Its Sprite 9 children are
   // static: move only the decorative side strips inside that animated owner.
   PM_StorageEdges(screen,base,left,right);
   PM_FrontFrame(screen.mcOverlay,left,ratio);
   if(screen.__pmSaveReportedRatio != ratio)
   {
      flash.external.ExternalInterface.call("PM_ReportUiCanvas",3,ratio,100);
      screen.__pmSaveReportedRatio = ratio;
   }
}
function PM_StorageEdges(screen, base, left, right)
{
   if(screen.mcStorageBackdrop == undefined) return;
   var backdrop = screen.mcStorageBackdrop.getInstanceAtDepth(base + 1);
   if(backdrop != undefined)
   {
      PM_RootShift(backdrop.getInstanceAtDepth(base + 7),right);
      PM_RootShift(backdrop.getInstanceAtDepth(base + 9),left);
      PM_RootShift(backdrop.getInstanceAtDepth(base + 11),left);
   }
}
// Each target is a single static root placement in the original resource.
// Move navigation as a unit, including its native hit region. Central option,
// save, image and graph content keeps its original transform and proportion.
var __pmMenuEdges = [
 ["protooptions",13,[1,11],[],[]],
 ["protoaudiooption",242,[7,17],[],[239]],
 ["protocontrolleroption",208,[7,17],[],[205]],
 ["protocontrolleroptiongamepad",27,[9,19],[],[23]],
 ["protocontrolleroptionkeyboard",31,[11,21],[],[28]],
 ["protovideooption",33,[9,10],[],[31]],
 ["protoextras",7,[1,3,5],[],[]],
 ["protocredits",31,[10,12,13],[],[28]],
 ["protopromocode",38,[9,11,13],[],[34]],
 ["protoselectdifficulty",9,[24,36],[],[3,5]],
 ["protocontroller",116,[114],[112],[1,108,110]],
 ["protoevents",160,[152],[150],[1,146,148]],
 ["protopauseaudio",334,[],[],[1]],
 ["protopausecontroller",300,[],[],[1]],
 ["protopausecontrollerkeyboard",151,[143],[141],[1,137,139]],
 ["protopausevideo",79,[],[],[1]],
 ["protopowerpurchase",130,[],[],[1]],
 ["protoquit",191,[],[],[1]],
 ["protosave",233,[],[],[1]],
 ["protostatistics",267,[265],[263],[1,259,261]],
 ["protowoi",109,[83],[85],[73]]
];
function PM_MenuEdges(screen, left, right, ratio)
{
   if(screen.mcOverlay == undefined) return;
   var i = 0;
   while(i < __pmMenuEdges.length)
   {
      var rule = __pmMenuEdges[i];
      if(screen.mScreenName == rule[0])
      {
         var base = screen.mcOverlay.getDepth() - rule[1];
         PM_Depths(screen,base,rule[2],left,1,0);
         PM_Depths(screen,base,rule[3],right,1,0);
         PM_StaticCanvas(screen,base,rule[4],ratio);
         // Every depth-1 pause canvas in this table has the verified logo child.
         if(rule[4][0] == 1) PM_PauseLogo(screen,base,ratio);
         PM_FrontFrame(screen.mcOverlay,left,ratio);
         if(screen.mScreenName == "protoselectdifficulty") PM_StorageEdges(screen,base,left,right);
         if(screen.__pmMenuReportedRatio != ratio)
         {
            flash.external.ExternalInterface.call("PM_ReportUiCanvas",5 + i,ratio,100);
            screen.__pmMenuReportedRatio = ratio;
         }
         return;
      }
      i++;
   }
}
function PM_Layout()
{
   var rect = Stage["visibleRect"];
   if(rect == undefined || rect.width < 1 || rect.height < 1) return;
   var left = Math.min(0,rect.x);
   var right = Math.max(0,rect.x + rect.width - 1280);
   var ratio = (1280 + right - left) / 1280;
   // Avoid touching original 16:9/4:3 layouts unless restoring a previous wide layout.
   if(ratio == 1 && !_root.__pmWasWide) return;
   _root.__pmWasWide = ratio != 1;
   // Global transitions must also cover frames with no screen in mcAnchor.
   PM_BlackFade(_root.mcBlackFade,ratio);
   var screen = _root.mcAnchor;
   if(screen == undefined) return;
   PM_FrontCanvas(screen,left,ratio);
   PM_SaveLoad(screen,left,right,ratio);
   PM_MenuEdges(screen,left,right,ratio);
   PM_PauseOverlay(screen.mcOverlay,left,right,ratio);
   PM_Map(screen,left,right,ratio);
   PM_Frame(screen.mcFrame,left,right,ratio);
   if(screen.mcOverlay != undefined) PM_Frame(screen.mcOverlay.mcFrame,left,right,ratio);
   _root.__pmVisibleWidth = rect.width;
}
// Diagnostics are independent of aspect ratio and never alter native layout,
// selection, event handlers or option values.
function PM_TraceMethod(screen, key)
{
   if(typeof screen[key] != "function" || screen[key].__pmOperationWrapper) return;
   var original = screen[key];
   var wrapper = function()
   {
      flash.external.ExternalInterface.call("PM_ReportAction","method",String(this.mScreenName),key,
         this.mSecondsWaiting == undefined ? -1 : this.mSecondsWaiting,
         arguments.length == 0 ? "none" : (typeof arguments[0] == "number" || typeof arguments[0] == "string" ? String(arguments[0]) : typeof arguments[0]));
      return original.apply(this,arguments);
   };
   wrapper.__pmOperationWrapper = true;
   screen[key] = wrapper;
}
function PM_TraceNavigation()
{
   var screen = _root.mcAnchor;
   var name = screen == undefined ? "none" : String(screen.mScreenName);
   if(_root.__pmTraceScreen != screen || _root.__pmTraceScreenName != name)
   {
      flash.external.ExternalInterface.call("PM_ReportAction","screen",name);
      _root.__pmTraceScreen = screen;
      _root.__pmTraceScreenName = name;
      _root.__pmTraceValues = new Object();
   }
   if(screen == undefined) return;
   PM_TraceMethod(screen,"PromptSaveSettings");
   PM_TraceMethod(screen,"SaveSettings");
   PM_TraceMethod(screen,"ShowPostConfirmDlg");
   PM_TraceMethod(screen,"CancelOnConfirm");
   PM_TraceMethod(screen,"OnSaveComplete");
   var values = _root.__pmTraceValues;
   for(var key in screen)
   {
      var control = screen[key];
      if(typeof control == "movieclip" && typeof control.mLogicalIndex == "number")
      {
         var index = control.mLogicalIndex;
         if(values["list:" + key] != index)
         {
            flash.external.ExternalInterface.call("PM_ReportAction","selection",name,key,index);
            values["list:" + key] = index;
         }
      }
   }
   var options = screen.mOptions;
   if(options == undefined) options = screen.mVideoOptions;
   var i = 0;
   while(options != undefined && i < options.length)
   {
      var option = options[i];
      if(option.mMovie != undefined && typeof option.mMovie.GetSelectedValue == "function")
      {
         var j = 0;
         var count = option.mDBValueCount == undefined ? 1 : option.mDBValueCount;
         while(j < count)
         {
            var value = option.mMovie.GetSelectedValue(j);
            var token = "option:" + i + ":" + j;
            if(values[token] != String(value))
            {
               flash.external.ExternalInterface.call("PM_ReportAction","option",name,
                  String(option.mDBName == null ? option.mLabel : option.mDBName),j,String(value));
               values[token] = String(value);
            }
            j++;
         }
      }
      i++;
   }
}
var __pmLayoutDriver = _root.createEmptyMovieClip("__PrototypeModernLayout",_root.getNextHighestDepth());
__pmLayoutDriver.onEnterFrame = function()
{
   _root.PM_TraceNavigation();
   _root.PM_Layout();
};
