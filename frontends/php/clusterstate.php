<?php

//require_once dirname(__FILE__).'/js/springy.js';
//require_once dirname(__FILE__).'/js/springygui.js';
require_once dirname(__FILE__).'/include/config.inc.php';
require_once dirname(__FILE__).'/include/hosts.inc.php';
require_once dirname(__FILE__).'/include/triggers.inc.php';
require_once dirname(__FILE__).'/include/items.inc.php';

$page['title'] = _('Cluster state');
$page['file'] = 'clusterstate.php';
$page['type'] = detect_page_type(PAGE_TYPE_HTML);
$page['scripts'] = ['layout.mode.js'];

CView::$has_web_layout_mode = true;
$page['web_layout_mode'] = CView::getLayoutMode();

define('ZBX_PAGE_DO_REFRESH', 1);
define('SHOW_TRIGGERS', 0);
define('SHOW_DATA', 1);

require_once dirname(__FILE__).'/include/page_header.php';

/*
 * Permissions
 */
if (getRequest('groupid') && !isReadableHostGroups([getRequest('groupid')])) {
	access_deny();
}

$server_list='';
//getting list of all the servers we have now
$servers = API::Proxy()->get([
    'output' => ['proxyid', 'host', 'status', 'lastaccess', 'tls_connect', 'tls_accept',
    'auto_compress','domains','error','useip'],
    'selectInterface' => ['interfaceid', 'dns', 'ip', 'useip', 'port'],
    'editable' => true,
    'preservekeys' => true,
   ]);
    
$topology=[];

foreach ($servers as $server) {
    if (HOST_STATUS_SERVER == $server['status']) {
            //	echo "Got true server ".$server['host'];
        $server_list=$server_list.$server['host'];
            //we need to fetch interface data as well
        if ($server['interface']['useip'])  
            $host= $server['interface']['ip']; 
        else 
            $host = $server['interface']['dns'];
        
        $zabbixServer = new CZabbixServer($host, $server['interface']['port'], ZBX_SOCKET_TIMEOUT, 0);
        $topology[$server['host']]=$zabbixServer->getTopology(CWebUser::getSessionCookie());
       
    }
} 

$data = [
    'topology' => $topology,
    'json_pretty_topology' => json_encode($topology, JSON_PRETTY_PRINT),
];

$overviewView = new CView('monitoring.cluster.state', $data);

// render view
$overviewView->render();
$overviewView->show();

require_once dirname(__FILE__).'/include/page_footer.php';
