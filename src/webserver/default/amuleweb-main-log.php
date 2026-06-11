<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">
<html>
<head>
<title>aMule control panel</title>
<meta http-equiv="Content-Type" content="text/html; charset=UTF-8">

<link href="style.css" rel="stylesheet" type="text/css">
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
    <td colspan="2">        <table class="w100p">
          <caption>
          AMULE LOG 
          </caption>
          <tr> 
            <td class="w24"><img src="images/tab_top_left.png" width="24" height="24"></td>
            <td class="tab-top">&nbsp;</td>
            <td class="w24"><img src="images/tab_top_right.png" width="24" height="24"></td>
          </tr>
          <tr> 
            <td class="w24 tab-left">&nbsp;</td>
            <td class="bg-white">

            <table class="w100p">
                <!--DWLayoutTable-->
                <tr class="va-top"> 
                  <td>
		<h1 style="display:inline;">aMule log</h1>
		<a href="log.php?rstlog=1" target="logframe" onclick="return confirm('Do you really want to reset aMule log?')">(Reset log)</a><br>
	<iframe class="w100p" height="400" name="logframe" src="log.php"></iframe>
                  </td>
                  </tr><tr>
                  <td>
		<h1 style="display:inline;">Serverinfo</h1>
		<a href="log.php?rstsrv=1" target="srvframe" onclick="return confirm('Do you really want to reset Serverinfo?')">(Reset Serverinfo)</a>
<iframe class="w100p" height="200" name="srvframe" src="log.php?show=srv"></iframe>
                  </td>
                  </tr>
                </tr>
              </table>
              </td>
            <td class="w24 tab-right">&nbsp;</td>
          </tr>
          <tr> 
            <td class="w24"><img src="images/tab_bottom_left.png" width="24" height="24"></td>
            <td class="tab-bottom">&nbsp;</td>
            <td class="w24"><img src="images/tab_bottom_right.png" width="24" height="24"></td>
          </tr>
        </table>
</td>
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
