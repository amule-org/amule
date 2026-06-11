<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">
<html>
<head>
<title>aMule control panel</title>
<meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
<?php
	if ( $_SESSION["auto_refresh"] > 0 ) {
		echo "<meta http-equiv=\"refresh\" content=\"", $_SESSION["auto_refresh"], '">';
	}

	amule_load_vars("stats_graph");

?>
<link href="style.css" rel="stylesheet" type="text/css">
<script language="JavaScript" type="text/JavaScript">
var openImg = new Image();
openImg.src = "tree-open.gif";
var closedImg = new Image();
closedImg.src = "tree-closed.gif";

function showBranch(branch){
	var objBranch = document.getElementById(branch).style;
	if(objBranch.display=="block")
		objBranch.display="none";
	else
		objBranch.display="block";
}

function swapFolder(img){
	objImg = document.getElementById(img);
	if(objImg.src.indexOf('tree-closed.gif')>-1)
		objImg.src = openImg.src;
	else
		objImg.src = closedImg.src;
}

</script>
</head>
<body class="main">
<table class="w100p h100p">
  <tr class="va-top"> 
    <td class="w143 logo-cell"><img src="images/logo.png" width="143" height="64"></td>
    <td class="w100p navbar-cell"> <table class="navbar-table">
        <tr> 
          <td><a class="navbutton nav-transfer" href="amuleweb-main-dload.php" title="Transfers"></a></td>
          <td><a class="navbutton nav-shared" href="amuleweb-main-shared.php" title="Shared files"></a></td>
          <td><a class="navbutton nav-search" href="amuleweb-main-search.php" title="Search"></a></td>
          <td><a class="navbutton nav-servers" href="amuleweb-main-servers.php" title="Servers"></a></td>
          <td><a class="navbutton nav-kad" href="amuleweb-main-kad.php" title="Kad"></a></td>
          <td><a class="navbutton nav-stats" href="amuleweb-main-stats.php" title="Statistics"></a></td>
          <td><img src="images/col.png"></td>
          <td class="w10"></td>
          <td class="w190 texteinv al-right"><a href="login.php">exit</a><br> 
            <a href="amuleweb-main-log.php">log &bull;</a> <a href="amuleweb-main-prefs.php">configuration</a></td>
          <td class="w10"></td>
        </tr>
      </table></td>
  </tr>
  <tr class="al-center va-top"> 
    <td colspan="2">
        <table class="w100p">
          <caption>
        STATISTICS 
        </caption>
          <tr> 
            <td class="w24"><img src="images/tab_top_left.png" width="24" height="24"></td>
            <td class="tab-top">&nbsp;</td>
            <td class="w24"><img src="images/tab_top_right.png" width="24" height="24"></td>
          </tr>
          <tr> 
            <td class="w24 tab-left">&nbsp;</td>
            
          <td class="bg-white"><table class="w100p">
              <tr class="va-top"> 
                <td rowspan="7">
<iframe name="stats" src="stats_tree.php" class="w100p noborder" height="630">liste</iframe></td>
                <td class="w500 al-right"><img src="amule_stats_download.png" width="500" height="200"></td>
              </tr>
              <tr class="va-top"> 
                <td class="w500 h15"> 
                  <div class="al-center">Download-Speed</div></td>
              </tr>
              <tr class="va-top"> 
                <td class="w500 al-right"><img src="amule_stats_upload.png" width="500" height="200" alt="" title="" /></td>
              </tr>
              <tr class="va-top"> 
                <td class="w500 h15"> 
                  <div class="al-center">Upload-Speed</div></td>
               </tr>
              <tr class="va-top"> 
                <td class="w500 al-right"><img src="amule_stats_conncount.png" width="500" height="200" alt="" title="" /></td>
              </tr>
              <tr class="va-top"> 
                <td class="w500 h15"> 
                  <div class="al-center">Connections</div></td>
              </tr>
            </table></td>
            <td class="w24 tab-right">&nbsp;</td>
          </tr>
          <tr> 
            <td class="w24"><img src="images/tab_bottom_left.png" width="24" height="24"></td>
            <td class="tab-bottom">&nbsp;</td>
            <td class="w24"><img src="images/tab_bottom_right.png" width="24" height="24"></td>
          </tr>
        </table></td>
  </tr>
  <tr class="va-bottom"> 
    <td class="h25" colspan="2"> <table class="w100p h40">
        <tr class="al-center va-middle"> 
          <td class="w50p"> <iframe name="stats" src="footer.php" height="35" class="w100p noborder">edklink</iframe> 
          </td>
          <td class="w50p"> <iframe name="stats" src="stats.php" height="35" class="w100p noborder">connection</iframe> 
          </td>
        </tr>
      </table></td>
  </tr>
</table>
</body>
</html>
