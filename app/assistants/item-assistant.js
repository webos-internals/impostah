function ItemAssistant(label, item, name, dbId, dirtyCallback) {

	this.label = label;
	this.item = item;
	this.name = name;
	this.dbId = dbId;
	this.dirtyCallback = dirtyCallback;

	// setup list model
	this.mainModel = {items:[]};
	
	// setup menu
	this.menuModel = {
		visible: true,
		items: [
			{ label: $L("Email"), command: 'do-email' },
			{ label: $L("Save To File"), command: 'do-save' },
			{ label: $L("Delete Item"), command: 'do-delete' },
			{ label: $L("Preferences"), command: 'do-prefs' },
			{ label: $L("Help"), command: 'do-help' }
		]
	};

	this.requestPalmService = false;
}

ItemAssistant.prototype.setup = function() {

	// setup menu
	this.controller.setupWidget(Mojo.Menu.appMenu, { omitDefaultItems: true }, this.menuModel);
	
	// get elements
	this.backElement = this.controller.get('icon');
	this.titleElement = this.controller.get('title');
	this.listElement = this.controller.get('mainList');

	this.titleElement.innerHTML = this.label;

    // handlers
	this.backTapHandler = this.backTap.bindAsEventListener(this);
    this.listTapHandler = this.listTap.bindAsEventListener(this);
    this.deleteItemAckHandler = this.deleteItemAck.bind(this);
    this.itemDeletedHandler = this.itemDeleted.bind(this);
	
	if ((typeof this.item) == 'object' && Object.isArray(this.item))
	{
		for (var n = 0; n < this.item.length; n++)
		{
			this.listData('# '+n, this.item[n]);
		}
	}
	else if ((typeof this.item) == 'object')
	{
		for (i in this.item)
		{
			this.listData(i, this.item[i]);
		}
	}
	else
	{
		this.mainModel.items.push({title: this.item});
	}

    // setup widgets
	this.controller.listen(this.backElement,  Mojo.Event.tap, this.backTapHandler);
    this.controller.setupWidget('mainList', {
			itemTemplate: "item/rowTemplate", swipeToDelete: false, reorderable: false }, this.mainModel);
    this.controller.listen(this.listElement, Mojo.Event.listTap, this.listTapHandler);

};

ItemAssistant.prototype.listData = function(label, value)
{
	switch (typeof value)
	{
		case 'string':
		case 'number':
		case 'boolean':
			this.mainModel.items.push({label: label, title: value, labelClass: 'left', titleClass: 'right'});
			break;
			
		case 'object':
			if (value)
			{
				if (Object.isArray(value))
				{
					this.mainModel.items.push({label: label, title: '<i>Array [ ... ]</i>', labelClass: 'left', titleClass: 'right', item: value});
				}
				else
				{
					this.mainModel.items.push({label: label, title: '<i>Object { ... }</i>', labelClass: 'left', titleClass: 'right', item: value});
				}
			}
			else
			{
				this.mainModel.items.push({label: label, title: '<i>null</i>', labelClass: 'left', titleClass: 'right'});
			}
			break;
			
		case 'function':
			this.mainModel.items.push({label: label, title: '<i>Function ...</i>', labelClass: 'left', titleClass: 'right', item: value});
			break;
			
		case 'undefined':
			this.mainModel.items.push({label: label, title: '<i>undefined</i>', labelClass: 'left', titleClass: 'right'});
			break;
			
		default:
			this.mainModel.items.push({label: label, title: '<strong>'+(typeof value)+'</strong>? '+value, labelClass: 'left', titleClass: 'right'});
			break;
	}
}

ItemAssistant.prototype.listTap = function(event)
{
	if (event.item.item) this.controller.stageController.pushScene("item", this.label+' - '+event.item.label, event.item.item, false);
};

ItemAssistant.prototype.deleteItem = function(dbId)
{
	this.controller.showAlertDialog({
			allowHTMLMessage:	true,
			title:				'Delete Item',
			message:			"Are you sure? This will permanently delete this database item.",
			choices:			[{label:$L("Delete"), value:'delete', type:'negative'},{label:$L("Cancel"), value:'cancel', type:'dismiss'}],
			onChoose:			this.deleteItemAckHandler
		});
};

ItemAssistant.prototype.deleteItemAck = function(value)
{
	if (value != "delete") {
		return;
	}

	if (this.requestPalmService) this.requestPalmService.cancel();
	this.requestPalmService = ImpostahService.impersonate(this.itemDeletedHandler,
														  "com.palm.configurator",
														  "com.palm.db",
														  "del", {"ids":[this.dbId], "purge":false});

};

ItemAssistant.prototype.itemDeleted = function(payload)
{
	if (this.requestPalmService) this.requestPalmService.cancel();
	this.requestPalmService = false;

	if (payload.returnValue === false) {
		this.errorMessage('<b>Service Error (deleteItem):</b><br>'+payload.errorText);
		return;
	}

	if (this.dirtyCallback) this.dirtyCallback();
	this.controller.stageController.popScene();
};

ItemAssistant.prototype.errorMessage = function(msg)
{
	this.controller.showAlertDialog(
	{
		allowHTMLMessage:	true,
		preventCancel:		true,
	    title:				'Impostah',
	    message:			msg,
	    choices:			[{label:$L("Ok"), value:'ok'}],
	    onChoose:			function(e){}
    });
};

ItemAssistant.prototype.backTap = function(event)
{
	this.controller.stageController.popScene();
};

ItemAssistant.prototype.handleCommand = function(event)
{
	if (event.type == Mojo.Event.command) {
		switch (event.command) {
		case 'do-email':
		email(this.label, "<pre>" + JSON.stringify(this.item, null, 4) + "</pre>");
		break;

		case 'do-save':
		this.controller.stageController.pushScene('json-save',{'object':this.item, 'filename':this.name+'.json'});
		break;
		
		case 'do-delete':
		if (this.dbId) {
			this.deleteItem(this.dbId);
		}
		break;
		
		case 'do-prefs':
		this.controller.stageController.pushScene('preferences');
		break;
				
		case 'do-help':
		this.controller.stageController.pushScene('help');
		break;
		}
	}
};

ItemAssistant.prototype.cleanup = function(event) {
	this.controller.stopListening(this.backElement,  Mojo.Event.tap, this.backTapHandler);
    this.controller.stopListening(this.listElement, Mojo.Event.listTap, this.listTapHandler);
};

// Local Variables:
// tab-width: 4
// End:
