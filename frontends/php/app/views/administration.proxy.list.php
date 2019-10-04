<?php
/*
** Zabbix
** Copyright (C) 2001-2019 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


if ($data['uncheck']) {
	uncheckTableRows('proxy');
}

$widget = (new CWidget())
	->setTitle(_('Cluster management'))
	->setControls((new CTag('nav', true,
		(new CList())
			->addItem(new CRedirectButton(_('Add server/proxy/domain'), 'zabbix.php?action=proxy.edit'))
		))
			->setAttribute('aria-label', _('Content controls'))
	)
	->addItem((new CFilter((new CUrl('zabbix.php'))->setArgument('action', 'proxy.list')))
		->setProfile($data['profileIdx'])
		->setActiveTab($data['active_tab'])
		->addFilterTab(_('Filter'), [
			(new CFormList())->addRow(_('Name'),
				(new CTextBox('filter_name', $data['filter']['name']))
					->setWidth(ZBX_TEXTAREA_FILTER_SMALL_WIDTH)
					->setAttribute('autofocus', 'autofocus')
			),
			(new CFormList())->addRow(_('Server type'),
				(new CRadioButtonList('filter_status', (int) $data['filter']['status']))
					->addValue(_('Any'), -1)
					->addValue(_('Proxy'), HOST_STATUS_PROXY_ACTIVE)
					->addValue(_('Monitoring domain'), HOST_STATUS_DOMAIN)
					->addValue(_('Server'), HOST_STATUS_SERVER)
					->setModern(true)
			)
		])
		->addVar('action', 'proxy.list')
	);

// create form
$proxyForm = (new CForm('get'))->setName('proxyForm');

// create table
$proxyTable = (new CTableInfo())
	->setHeader([
		(new CColHeader(
			(new CCheckBox('all_hosts'))
				->onClick("checkAll('".$proxyForm->getName()."', 'all_hosts', 'proxyids');")
		))->addClass(ZBX_STYLE_CELL_WIDTH),
		make_sorting_header(_('Name'), 'host', $data['sort'], $data['sortorder']),
		_('Type'),
		_('Encryption'),
		_('Compression'),
		_('Last seen (age)'),
		_('Host count'),
		_('Item count'),
		_('Required performance (vps)'),
		_('Hosts')
	]);

foreach ($data['proxies'] as $proxy) {
	$hosts = [];
	$i = 0;
	//only show list of related hosts for domains
	if (HOST_STATUS_DOMAIN == $proxy['status']  ) {
		foreach ($proxy['hosts'] as $host) {
			if (++$i > $data['config']['max_in_table']) {
				$hosts[] = ' &hellip;';

				break;
			}

			switch ($host['status']) {
				case HOST_STATUS_DOMAIN:
					$style = ZBX_STYLE_GREY;
					break;
				case HOST_STATUS_MONITORED:
					$style = null;
					break;
				case HOST_STATUS_TEMPLATE:
					$style = ZBX_STYLE_GREY;
					break;
				default:
					$style = ZBX_STYLE_RED;
			}

			if ($hosts) {
				$hosts[] = ', ';
			}

			$hosts[] = (new CLink($host['name'], 'hosts.php?form=update&hostid='.$host['hostid']))->addClass($style);
		}
	} else {
		//todo: fetch domains in the CProxy instead of here, just use the options
		//show list of related domains
		if ($proxy['error']) {
			//fetching list of domains
			$domain_ids=explode(',',$proxy['error']);

			$tdomains=API::Proxy()->get([
				'proxyids' => $domain_ids,
				'countOutput' => false,
				'editable' => true,
			]);
		
			
			foreach($tdomains as $domain) {		
			
				if ($hosts) {
					$hosts[] = ', ';
				}
				$hosts[] = ($domain['host']);
			}
		}
			
	}

	$name = new CLink($proxy['host'], 'zabbix.php?action=proxy.edit&proxyid='.$proxy['proxyid']);

	// encryption
	$in_encryption = '';
	$out_encryption = '';

	if ($proxy['status'] == HOST_STATUS_PROXY_PASSIVE) {
		// input encryption
		if ($proxy['tls_connect'] == HOST_ENCRYPTION_NONE) {
			$in_encryption = (new CSpan(_('None')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}
		elseif ($proxy['tls_connect'] == HOST_ENCRYPTION_PSK) {
			$in_encryption = (new CSpan(_('PSK')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}
		else {
			$in_encryption = (new CSpan(_('CERT')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}
	}
	else {
		// output encryption
		$out_encryption_array = [];
		if (($proxy['tls_accept'] & HOST_ENCRYPTION_NONE) == HOST_ENCRYPTION_NONE) {
			$out_encryption_array[] = (new CSpan(_('None')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}
		if (($proxy['tls_accept'] & HOST_ENCRYPTION_PSK) == HOST_ENCRYPTION_PSK) {
			$out_encryption_array[] = (new CSpan(_('PSK')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}
		if (($proxy['tls_accept'] & HOST_ENCRYPTION_CERTIFICATE) == HOST_ENCRYPTION_CERTIFICATE) {
			$out_encryption_array[] = (new CSpan(_('CERT')))->addClass(ZBX_STYLE_STATUS_GREEN);
		}

		$out_encryption = (new CDiv($out_encryption_array))->addClass(ZBX_STYLE_STATUS_CONTAINER);
	}

	$proxy_statuses= [
		HOST_STATUS_PROXY_ACTIVE => _('Active Proxy'),
		HOST_STATUS_PROXY_PASSIVE => _('Passive Proxy'),
		HOST_STATUS_DOMAIN =>  _('Monitoring Domain'),
		HOST_STATUS_SERVER =>  _('Server')
	];

	$proxyTable->addRow([
		new CCheckBox('proxyids['.$proxy['proxyid'].']', $proxy['proxyid']),
		(new CCol($name))->addClass(ZBX_STYLE_NOWRAP),
		//todo: fix to real names, as for now just an index will be shown
		$proxy_statuses[$proxy['status']],
		$proxy['status'] == HOST_STATUS_PROXY_ACTIVE ? $out_encryption : $in_encryption,
		($proxy['status'] ==HOST_STATUS_PROXY_ACTIVE) ? (
		($proxy['auto_compress'] == HOST_COMPRESSION_ON)
			? (new CSpan(_('On')))->addClass(ZBX_STYLE_STATUS_GREEN)
			: (new CSpan(_('Off')))->addClass(ZBX_STYLE_STATUS_GREY)) : '',
		($proxy['status'] !=HOST_STATUS_DOMAIN) ? 
		($proxy['lastaccess'] == 0)
			? (new CSpan(_('Never')))->addClass(ZBX_STYLE_RED)
			: zbx_date2age($proxy['lastaccess']) : '',
		array_key_exists('host_count', $proxy) ? $proxy['host_count'] : '',
		array_key_exists('item_count', $proxy) ? $proxy['item_count'] : '',
		array_key_exists('vps_total', $proxy) ? $proxy['vps_total'] : '',
		$hosts ? $hosts : ''
	]);
}

// append table to form
$proxyForm->addItem([
	$proxyTable,
	$data['paging'],
	new CActionButtonList('action', 'proxyids', [
		'proxy.delete' => ['name' => _('Delete'), 'confirm' => _('Delete selected severs/domains?')]
	], 'proxy')
]);

// append form to widget
$widget->addItem($proxyForm)->show();
