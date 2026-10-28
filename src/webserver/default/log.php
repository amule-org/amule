<!doctype html>
<html><head>
<title>aMule control panel</title>
<meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
<?php
	if ( $_SESSION["auto_refresh"] > 0 ) {
		echo "<meta http-equiv=\"refresh\" content=\"", $_SESSION["auto_refresh"], '">';
	}

?>
<style type="text/css">
body {
	margin-left: 0px;
	margin-top: 0px;
	margin-right: 0px;
	margin-bottom: 0px;
	background-color: #FFFFFF;
	font-size: small;
	font-family: monospace;
	color: #000000;
}
</style>
</head>
<body>
<pre>
<?php
	$is_admin = ($_SESSION["guest_login"] == 0);
	$rstsrv = $is_admin ? $HTTP_GET_VARS['rstsrv'] : 0;
	$rstlog = $is_admin ? $HTTP_GET_VARS['rstlog'] : 0;
	if ("srv" == $HTTP_GET_VARS['show'] || 1 == $HTTP_GET_VARS['rstsrv']) {
		echo htmlspecialchars(amule_get_serverinfo($rstsrv));
	 }else {
		echo htmlspecialchars(amule_get_log($rstlog));
	}
?>
</pre>
</body>
</html>
