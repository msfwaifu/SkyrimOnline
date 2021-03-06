#include "InventoryReference.h"
#include "GameObjects.h"
#include "GameAPI.h"

void WriteToExtraDataList(BaseExtraList* from, BaseExtraList* to)
{
	ASSERT(to || from);

	if (from) {
		if (to) {
			to->m_data = from->m_data;
			memcpy(to->m_presenceBitfield, from->m_presenceBitfield, 0xC);
		}
	}
	else if (to) {
		to->m_data = NULL;
		memset(to->m_presenceBitfield, 0, 0xC);
	}
}

std::map<UInt32, InventoryReference*> InventoryReference::s_refmap;

InventoryReference::InventoryReference(TESObjectREFR* container, const Data &data, bool bValidate) : m_containerRef(container), m_bDoValidation(bValidate), m_bRemoved(false)
{
	m_tempRef = TESObjectREFR::Create(false);
	SetData(data);
	s_refmap[m_tempRef->refID] = this;
}

InventoryReference::~InventoryReference()
{
	if (m_data.type) {
		Release();	
	}

	// note: TESObjectREFR::Destroy() frees up the formID for reuse
	if (m_tempRef) {
		s_refmap.erase(m_tempRef->refID);
		m_tempRef->Destroy(false);
	}

	// remove unnecessary extra data, consolidate identical stacks
	if (m_containerRef) {
		ExtraContainerChanges* xChanges = (ExtraContainerChanges*)m_containerRef->baseExtraList.GetByType(kExtraData_ContainerChanges);
		if (xChanges) {
			xChanges->Cleanup();
		}
	}
}

void InventoryReference::Release()
{
	DoDeferredActions();
	SetData(Data(NULL, NULL, NULL));
}

bool InventoryReference::SetData(const Data &data)
{
	ASSERT(m_tempRef);

	m_bRemoved = false;
	m_tempRef->baseForm = data.type;
	m_data = data;
	ExtraDataList* list = data.extendData ? data.extendData->data : NULL;
	WriteToExtraDataList(list, &m_tempRef->baseExtraList);
	return true;
}

bool InventoryReference::WriteRefDataToContainer()
{
	if (!m_tempRef || !Validate())
		return false;
	else if (m_bRemoved)
		return true;

	ExtraContainerChanges::EntryExtendData* data = m_data.extendData;
	if (!data->data && m_tempRef->baseExtraList.m_data) {	// some data was added to ref, create list to hold it
		data->data = ExtraDataList::Create();
	}

	if (data->data) {
		WriteToExtraDataList(&m_tempRef->baseExtraList, data->data);

#if _DEBUG
		data->data->DebugDump();
#endif
	}

	return true;
}

SInt16 InventoryReference::GetCount()
{
	SInt16 count = 0;
	if (Validate()) {
		count = 1;
		if (m_data.extendData && m_data.extendData->data) {
			ExtraCount* xCount = (ExtraCount*)m_data.extendData->data->GetByType(kExtraData_Count);
			if (xCount) {
				count = xCount->count;
			}
		}
	}

	if (count < 0) {
		DEBUG_PRINT("Warning: InventoryReference::GetCount() found an object with a negative count (%d)", count);
	}

	return count;
}

bool InventoryReference::Validate()
{
	// it is possible that an inventory reference is created, and then commands like RemoveItem are used which modify the 
	// ExtraContainerChanges, potentially invalidating the InventoryReference::Data pointers
	// if m_bValidate is true, check for this occurrence before doing stuff with the temp ref
	// doing this in foreach loops is going to make them really slooooow.

	if (!m_bDoValidation)
		return true;

	ExtraContainerChanges* xChanges = (ExtraContainerChanges*)m_containerRef->baseExtraList.GetByType(kExtraData_ContainerChanges);
	if (xChanges && xChanges->data) {
		for (ExtraContainerChanges::Entry* cur = xChanges->data->objList; cur; cur = cur->next) {
			if (cur == m_data.entry && cur->data && cur->data == m_data.entry->data && cur->data->type == m_data.entry->data->type) {
				for (ExtraContainerChanges::EntryExtendData* ed = cur->data->extendData; cur; cur = cur->next) {
					if (ed == m_data.extendData) {
						return true;
					}
				}
			}
		}
	}

	return false;
}

InventoryReference* InventoryReference::GetForRefID(UInt32 refID)
{
	std::map<UInt32, InventoryReference*>::iterator found = s_refmap.find(refID);
	if (found != s_refmap.end() && found->second->Validate()) {
		return found->second;
	}

	return NULL;
}

void InventoryReference::Clean()
{
	std::map<UInt32, InventoryReference*>::iterator iter = s_refmap.begin();
	while (iter != s_refmap.end()) {
		InventoryReference* refr = iter->second;
		UInt32 refid = iter->first;
		s_refmap.erase(refid);
		delete refr;		
		iter = s_refmap.begin();
	}
}

bool InventoryReference::RemoveFromContainer()
{
	if (Validate() && m_tempRef && m_containerRef) {
		if (m_data.extendData && m_data.extendData->data && m_data.extendData->data->IsWorn()) {
			QueueAction(new DeferredRemoveAction(m_data));
			return true;
		}
		SetRemoved();
		return m_data.entry->Remove(m_data.extendData, true);
	}
	return false;
}

bool InventoryReference::MoveToContainer(TESObjectREFR* dest)
{
	ExtraContainerChanges* xChanges = ExtraContainerChanges::GetForRef(dest);
	if (xChanges && Validate() && m_tempRef && m_containerRef) {
		if (m_data.extendData && m_data.extendData->data && m_data.extendData->data->IsWorn()) {
			QueueAction(new DeferredRemoveAction(m_data, dest));
			return true;
		}
		else if (m_data.entry->Remove(m_data.extendData, false)) {
			SetRemoved();
			ExtraDataList* newDataList = ExtraDataList::Create();
			newDataList->Copy(&m_tempRef->baseExtraList);
			m_tempRef->baseExtraList.RemoveAll();
			return xChanges->Add(m_tempRef->baseForm, newDataList) ? true : false;
		}
	}
	return false;
}

bool InventoryReference::CopyToContainer(TESObjectREFR* dest)
{
	ExtraContainerChanges* xChanges = ExtraContainerChanges::GetForRef(dest);
	if (xChanges && Validate() && m_tempRef) {
		ExtraDataList* newDataList = ExtraDataList::Create();
		newDataList->Copy(&m_tempRef->baseExtraList);

		// if worn, mark as unequipped
		if (!newDataList->RemoveByType(kExtraData_Worn)) {
			newDataList->RemoveByType(kExtraData_WornLeft);
		}

		return xChanges->Add(m_tempRef->baseForm, newDataList) ? true : false;
	}

	return false;
}

bool InventoryReference::SetEquipped(bool bEquipped)
{
	if (m_data.extendData && m_data.extendData->data) { 
		if (m_data.extendData->data->IsWorn() != bEquipped) {
			QueueAction(new DeferredEquipAction(m_data));
			return true;
		}
	}
	else if (bEquipped) {
		QueueAction(new DeferredEquipAction(m_data));
		return true;
	}
	return false;
}

void InventoryReference::QueueAction(DeferredAction* action)
{
	m_deferredActions.push(action);
}

void InventoryReference::DoDeferredActions()
{
	while (m_deferredActions.size()) {
		DeferredAction* action = m_deferredActions.front();
		SetData(action->Data());
		if (Validate() && GetContainer()) {
			if (!action->Execute(this)) {
				DEBUG_PRINT("InventoryReference::DeferredAction::Execute() failed");
			}
		}

		delete action;
		m_deferredActions.pop();
	}
}

bool InventoryReference::DeferredEquipAction::Execute(InventoryReference* iref)
{
	const InventoryReference::Data& data = Data();
	Actor* actor = OBLIVION_CAST(iref->GetContainer(), TESObjectREFR, Actor);
	if (actor) {
		if (data.extendData && data.extendData->data && data.extendData->data->IsWorn()) {
			actor->UnequipItem(data.type, 1, data.extendData->data, 0, false, 0);
		}
		else {
			UInt16 count = 1;
			if (data.type->typeID == kFormType_Ammo) {
				// equip a *stack* of arrows
				count = iref->GetCount();
			}
			actor->EquipItem(data.type, count, data.extendData ? data.extendData->data : NULL, 1, false);
		}
		return true;
	}
	return false;
}

bool InventoryReference::DeferredRemoveAction::Execute(InventoryReference* iref)
{
	TESObjectREFR* containerRef = iref->GetContainer();
	const InventoryReference::Data& data = Data();
	if (containerRef) {
		containerRef->RemoveItem(data.type, data.extendData ? data.extendData->data : NULL, iref->GetCount(), 0, 0, m_target, 0, 0, 1, 0);
		iref->SetRemoved();
		iref->SetData(InventoryReference::Data(NULL, NULL, NULL));
		return true;
	}
	return false;
}

void InventoryReference::Data::CreateForUnextendedEntry(ExtraContainerChanges::Entry* entry, SInt32 totalCount, std::vector<Data> &dataOut)
{
	if (totalCount < 1) {
		return;
	}

	// create new EntryExtendData to hold ExtraDataList for first stack
	ExtraContainerChanges::EntryExtendData* curExtend = entry->data->extendData;
	if (!curExtend) {
		curExtend = ExtraContainerChanges::EntryExtendData::Create(ExtraDataList::Create());
		entry->data->extendData = curExtend;
	}
	else {
		for (; curExtend->next; curExtend = curExtend->next) {
			//
		}
		curExtend->next = ExtraContainerChanges::EntryExtendData::Create(ExtraDataList::Create());
		curExtend = curExtend->next;
	}

	// create stacks of max 32767 as that's the max ExtraCount::count can hold
	while (totalCount > 32767) {
		curExtend->data->Add(ExtraCount::Create(32767));
		dataOut.push_back(Data(entry->data->type, entry, curExtend));
		totalCount -= 32767;
		if (totalCount > 0) {
			// need another new EntryExtendData for next stack
			curExtend->next = ExtraContainerChanges::EntryExtendData::Create(ExtraDataList::Create());
			curExtend = curExtend->next;
		}
	}

	if (totalCount > 0) {
		if (totalCount > 1) {
			curExtend->data->Add(ExtraCount::Create(totalCount));
		}
		dataOut.push_back(Data(entry->data->type, entry, curExtend));
	}
}


