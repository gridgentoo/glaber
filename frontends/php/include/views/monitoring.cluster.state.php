<?php


// header right
$web_layout_mode = CView::getLayoutMode();

$widget = (new CWidget())
	->setTitle(_('Servers state'))
    ->setWebLayoutMode($web_layout_mode);

    $serverlist='';
    $downservers='';
    $edges='';

    
//var_dump($data['topology']);

//
//$hosts=API::Host()->get([
  //  'output' => ['host,'],
  //  'hostids' => $topology['hosts'],
//]);


//var_dump($hosts);

foreach ($data['topology'] as $server=>$topology) {
    $widget->addItem(new CTag('h1', true, _($server)));
    if (!$topology) {
       $widget->addItem(
           new CTag('H1', true, (new CSpan('DOWN (NO DATA)'))->addClass(ZBX_STYLE_RED) ) 
        );
        
        if ($downservers) $downservers.=",'$server',"; else $downservers.="'$server'";
    } else {
        $hh='';

        $widget->addItem(new CSpan("topology version: ".$topology['cluster_topology_version']));
        $widget->addItem( 
            new CTag('H1', true, (new CSpan('OK'))->addClass(ZBX_STYLE_GREEN) ) 
         );
        
        $widget->addItem("Servers <br>")    ;

        foreach ($topology['servers'] as $tserver) {
                //var_dump($tserver);
                
                if ( isset($tserver['hosts'])  && sizeof($tserver['hosts'])  > 0) 
                     $server_class=ZBX_STYLE_GREEN;
                else $server_class=ZBX_STYLE_RED;
                $hostcount = isset($tserver['hosts'])?sizeof($tserver['hosts']):"N/A";
                $widget->addItem( "<br/>".(new CSpan($tserver['host']." (". sizeof($tserver['hosts'])." hosts active)"))->addClass($server_class)  . "<br/>" );
            //var_dump($tserver);
            //$widget->addItem("Hosts in the server <br>")    ;

            foreach ($tserver['hosts'] as $host) {
                    $hh.=$host.", ";
            }
            if ( sizeof($tserver['hosts'])  > 0 && isset($tserver['proxies'])) {
                $widget->addItem("  proxies<br/>");
                foreach ($tserver['proxies'] as $proxy) {
                    $widget->addItem( "<br/>".(new CSpan("&nbsp;&nbsp;&nbsp;".$proxy['proxy_id']." (". sizeof($proxy['hosts'])." hosts active)"))->addClass(ZBX_STYLE_YELLOW)  . "<br/>" );        
                }
            }
            //var_dump($topology['hosts']);
           // $widget->addItem(new CSpan($hh));
            $hh='';
                
        }

        ///
        if ($serverlist) $serverlist.=",'$server',"; else $serverlist.="'$server'";

        foreach($topology['servers'] as $tserver) {
            //adding link from $server to $server
            $edges.="['$server','".$tserver['host']."'],\n";
        }
    }
    $widget->addItem("<br/>");
 
}   
$widget->addItem("<pre>".$data['json_pretty_topology']."</pre>");
          

$gr1="   ";
           
            $widget->addItem($gr1);

     
//error("Hello world");      
//need to add 
//1. graph with servers here
//2. download and show server topology
//3. if there are more then one server is running. then do a topology analyze

/*
    $http_popup_form_list->addRow(_('Query fields'),
	(new CDiv(
		(new CTable())
			->addClass('httpconf-dynamic-row')
			->addStyle('width: 100%;')
			->setAttribute('data-type', 'query_fields')
			->setHeader(['', _('Name'), '', _('Value'), ''])
			->addRow((new CRow([
				(new CCol(
					(new CButton(null, _('Add')))
						->addClass('element-table-add')
						->addClass(ZBX_STYLE_BTN_LINK)
				))->setColSpan(5)
			])))
	))
		->addClass(ZBX_STYLE_TABLE_FORMS_SEPARATOR)
		->addStyle('min-width: '.ZBX_TEXTAREA_BIG_WIDTH . 'px;'),
		'query-fields-row'
);    

*/

return $widget;
