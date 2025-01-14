// Copyright 2022 Pentangle Studio Licensed under the Apache License, Version 2.0 (the «License»);

#include "Graph/InputSequenceGraphSchema.h"
#include "Graph/InputSequenceGraph.h"
#include "Graph/InputSequenceGraphFactories.h"
#include "ConnectionDrawingPolicy.h"

#include "Graph/InputSequenceGraphNode_GoToStart.h"
#include "Graph/InputSequenceGraphNode_Hub.h"
#include "Graph/InputSequenceGraphNode_Input.h"
#include "Graph/InputSequenceGraphNode_Press.h"
#include "Graph/InputSequenceGraphNode_Release.h"
#include "Graph/InputSequenceGraphNode_Start.h"
#include "Graph/InputSequenceGraphNode_Axis.h"
#include "Graph/SInputSequenceGraphNode_Dynamic.h"

#include "KismetPins/SGraphPinExec.h"
#include "Graph/SGraphPin_2DAxis.h"
#include "Graph/SGraphPin_Action.h"
#include "Graph/SGraphPin_Add.h"
#include "Graph/SGraphPin_Axis.h"
#include "Graph/SGraphPin_HubAdd.h"
#include "Graph/SGraphPin_HubExec.h"

#include "InputSequenceAssetEditor.h"
#include "InputSequenceAsset.h"
#include "GraphEditorActions.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Commands/GenericCommands.h"

#include "Settings/EditorStyleSettings.h"
#include "GameFramework/InputSettings.h"
#include "SGraphActionMenu.h"
#include "SPinTypeSelector.h"
#include "SLevelOfDetailBranchNode.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "UObject/ObjectSaveContext.h"
#include "SGraphPanel.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "InputAction.h"

const FString separator = " ^ ";

template<class T>
TSharedPtr<T> AddNewActionAs(FGraphContextMenuBuilder& ContextMenuBuilder, const FText& Category, const FText& MenuDesc, const FText& Tooltip, const int32 Grouping = 0)
{
	TSharedPtr<T> Action(new T(Category, MenuDesc, Tooltip, Grouping));
	ContextMenuBuilder.AddAction(Action);
	return Action;
}

template<class T>
void AddNewActionIfHasNo(FGraphContextMenuBuilder& ContextMenuBuilder, const FText& Category, const FText& MenuDesc, const FText& Tooltip, const int32 Grouping = 0)
{
	for (auto NodeIt = ContextMenuBuilder.CurrentGraph->Nodes.CreateConstIterator(); NodeIt; ++NodeIt)
	{
		UEdGraphNode* Node = *NodeIt;
		if (const T* castedNode = Cast<T>(Node)) return;
	}

	TSharedPtr<FInputSequenceGraphSchemaAction_NewNode> Action = AddNewActionAs<FInputSequenceGraphSchemaAction_NewNode>(ContextMenuBuilder, Category, MenuDesc, Tooltip, Grouping);
	Action->NodeTemplate = NewObject<T>(ContextMenuBuilder.OwnerOfTemporaries);
}

void AddPin(UEdGraphNode* node, FName category, FName pinName, const UEdGraphNode::FCreatePinParams& params, UObject* inputActionObj)
{
	UEdGraphPin* graphPin = node->CreatePin(EGPD_Output, category, pinName, params);

	if (inputActionObj)
	{
		if (UInputSequenceGraphNode_Input* inputNode = Cast<UInputSequenceGraphNode_Input>(node))
		{
			inputNode->GetPinsInputActions().Add(pinName, inputActionObj);
		}
	}

	node->Modify();

	if (UInputSequenceGraphNode_Dynamic* dynamicNode = Cast<UInputSequenceGraphNode_Dynamic>(node))
	{
		dynamicNode->OnUpdateGraphNode.ExecuteIfBound();
	}
}

class FInputSequenceConnectionDrawingPolicy : public FConnectionDrawingPolicy
{
public:
	FInputSequenceConnectionDrawingPolicy(int32 InBackLayerID, int32 InFrontLayerID, float ZoomFactor, const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj)
		: FConnectionDrawingPolicy(InBackLayerID, InFrontLayerID, ZoomFactor, InClippingRect, InDrawElements)
		, GraphObj(InGraphObj)
	{}

	virtual void DetermineWiringStyle(UEdGraphPin* OutputPin, UEdGraphPin* InputPin, /*inout*/ FConnectionParams& Params) override
	{
		FConnectionDrawingPolicy::DetermineWiringStyle(OutputPin, InputPin, Params);

		if (OutputPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec)
		{
			Params.WireThickness = 4;
		}
		else
		{
			Params.bUserFlag1 = true;
		}

		const bool bDeemphasizeUnhoveredPins = HoveredPins.Num() > 0;
		if (bDeemphasizeUnhoveredPins)
		{
			ApplyHoverDeemphasis(OutputPin, InputPin, /*inout*/ Params.WireThickness, /*inout*/ Params.WireColor);
		}
	}

	virtual void DrawSplineWithArrow(const FVector2D& StartPoint, const FVector2D& EndPoint, const FConnectionParams& Params) override
	{
		DrawConnection(
			WireLayerID,
			StartPoint,
			EndPoint,
			Params);

		// Draw the arrow
		if (ArrowImage != nullptr && Params.bUserFlag1)
		{
			FVector2D ArrowPoint = EndPoint - ArrowRadius;

			FSlateDrawElement::MakeBox(
				DrawElementsList,
				ArrowLayerID,
				FPaintGeometry(ArrowPoint, ArrowImage->ImageSize * ZoomFactor, ZoomFactor),
				ArrowImage,
				ESlateDrawEffect::None,
				Params.WireColor
			);
		}
	}

protected:
	UEdGraph* GraphObj;
	TMap<UEdGraphNode*, int32> NodeWidgetMap;
};

TSharedPtr<SGraphNode> FInputSequenceGraphNodeFactory::CreateNode(UEdGraphNode* InNode) const
{
	if (UInputSequenceGraphNode_Dynamic* stateNode = Cast<UInputSequenceGraphNode_Dynamic>(InNode))
	{
		return SNew(SInputSequenceGraphNode_Dynamic, stateNode);
	}

	return nullptr;
}

TSharedPtr<SGraphPin> FInputSequenceGraphPinFactory::CreatePin(UEdGraphPin* InPin) const
{
	if (InPin->GetSchema()->IsA<UInputSequenceGraphSchema>())
	{
		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec)
		{
			if (InPin->GetOwningNode() && InPin->GetOwningNode()->IsA<UInputSequenceGraphNode_Hub>()) return SNew(SGraphPin_HubExec, InPin);

			return SNew(SGraphPinExec, InPin);
		}

		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action) return SNew(SGraphPin_Action, InPin);

		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Add) return SNew(SGraphPin_Add, InPin);

		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_2DAxis) return SNew(SGraphPin_2DAxis, InPin);

		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Axis) return SNew(SGraphPin_Axis, InPin);

		if (InPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_HubAdd) return SNew(SGraphPin_HubAdd, InPin);
	}

	return nullptr;
}

FConnectionDrawingPolicy* FInputSequenceGraphPinConnectionFactory::CreateConnectionPolicy(const UEdGraphSchema* Schema, int32 InBackLayerID, int32 InFrontLayerID, float ZoomFactor, const class FSlateRect& InClippingRect, class FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj) const
{
	if (Schema->IsA<UInputSequenceGraphSchema>())
	{
		return new FInputSequenceConnectionDrawingPolicy(InBackLayerID, InFrontLayerID, ZoomFactor, InClippingRect, InDrawElements, InGraphObj);;
	}

	return nullptr;
}

UInputSequenceGraph::UInputSequenceGraph(const FObjectInitializer& ObjectInitializer) :Super(ObjectInitializer)
{
	Schema = UInputSequenceGraphSchema::StaticClass();
}

void GetNextNodes(UEdGraphNode* node, TArray<UEdGraphNode*>& outNextNodes)
{
	if (node)
	{
		for (UEdGraphPin* pin : node->Pins)
		{
			if (pin && pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec && pin->Direction == EGPD_Output)
			{
				for (UEdGraphPin* linkedPin : pin->LinkedTo)
				{
					if (linkedPin)
					{
						if (UEdGraphNode* linkedNode = linkedPin->GetOwningNode())
						{
							if (UInputSequenceGraphNode_Hub* linkedHub = Cast<UInputSequenceGraphNode_Hub>(linkedNode))
							{
								GetNextNodes(linkedHub, outNextNodes);
							}
							else
							{
								outNextNodes.Add(linkedNode);
							}
						}
					}
				}
			}
		}
	}
}

void UInputSequenceGraph::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	if (UInputSequenceAsset* inputSequenceAsset = GetTypedOuter<UInputSequenceAsset>())
	{
		inputSequenceAsset->States.Empty();

		struct FGuidCollection
		{
			TArray<FGuid> Guids;
		};

		struct FNodesQueueEntry
		{
			UEdGraphNode* Node = nullptr;
			int32 DepthIndex = 0;
			int32 FirstLayerParentIndex = INDEX_NONE;
			TSet<FName> PressedActions;

			FNodesQueueEntry(UEdGraphNode* const node = nullptr, const int depthIndex = 0, const int32 firstLayerParentIndex = INDEX_NONE, const TSet<FName>& pressedActions = {})
				: Node(node)
				, DepthIndex(depthIndex)
				, FirstLayerParentIndex(firstLayerParentIndex)
				, PressedActions(pressedActions)
			{}
		};

		TArray<FGuidCollection> linkedNodesMapping;

		TMap<FGuid, int> indexMapping;

		TQueue<FNodesQueueEntry> graphNodesQueue;
		graphNodesQueue.Enqueue(FNodesQueueEntry(Nodes[0], 0, INDEX_NONE));

		FNodesQueueEntry currentGraphNodeEntry(nullptr, INDEX_NONE);
		while (graphNodesQueue.Dequeue(currentGraphNodeEntry))
		{
			int32 emplacedIndex = inputSequenceAsset->States.Emplace();
			indexMapping.Add(currentGraphNodeEntry.Node->NodeGuid, emplacedIndex);

			linkedNodesMapping.Emplace();

			FInputSequenceState& state = inputSequenceAsset->States[emplacedIndex];
			state.DepthIndex = currentGraphNodeEntry.DepthIndex;
			state.FirstLayerParentIndex = currentGraphNodeEntry.FirstLayerParentIndex;
			state.PressedActions = currentGraphNodeEntry.PressedActions;

			TSet<FName> pressedActions = currentGraphNodeEntry.PressedActions;

			if (UInputSequenceGraphNode_Input* inputNode = Cast<UInputSequenceGraphNode_Input>(currentGraphNodeEntry.Node))
			{
				state.IsInputNode = 1;

				state.StateObject = inputNode->GetStateObject();
				state.StateContext = inputNode->GetStateContext();
				state.EnterEventClasses = inputNode->GetEnterEventClasses();
				state.PassEventClasses = inputNode->GetPassEventClasses();
				state.ResetEventClasses = inputNode->GetResetEventClasses();

				state.isOverridingRequirePreciseMatch = inputNode->IsOverridingRequirePreciseMatch();
				state.requirePreciseMatch = inputNode->RequirePreciseMatch();

				state.isOverridingResetAfterTime = inputNode->IsOverridingResetAfterTime();
				state.isResetAfterTime = inputNode->IsResetAfterTime();

				state.TimeParam = inputNode->GetResetAfterTime();

				if (UInputSequenceGraphNode_Press* pressNode = Cast<UInputSequenceGraphNode_Press>(currentGraphNodeEntry.Node))
				{
					for (UEdGraphPin* pin : pressNode->Pins)
					{
						if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action)
						{
							if (pin->LinkedTo.Num() == 0)
							{
								static FInputActionState waitForPressAndRelease({ IE_Pressed, IE_Released });
								state.InputActions.Add(pin->PinName, waitForPressAndRelease);
							}
							else
							{
								static FInputActionState waitForPress({ IE_Pressed });
								state.InputActions.Add(pin->PinName, waitForPress);

								pressedActions.Add(pin->PinName);
							}
						}
					}
				}
				else if (UInputSequenceGraphNode_Release* releaseNode = Cast<UInputSequenceGraphNode_Release>(currentGraphNodeEntry.Node))
				{
					for (UEdGraphPin* pin : releaseNode->Pins)
					{
						if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action)
						{
							static FInputActionState waitForRelease({ IE_Released });
							state.InputActions.Add(pin->PinName, waitForRelease);
							state.PressedActions.Remove(pin->PinName);
							
							pressedActions.Remove(pin->PinName);
						}
					}

					state.canBePassedAfterTime = releaseNode->CanBePassedAfterTime();
					if (state.canBePassedAfterTime)
					{
						state.TimeParam = releaseNode->GetPassedAfterTime();
					}
				}
				else if (UInputSequenceGraphNode_Axis* axisNode = Cast<UInputSequenceGraphNode_Axis>(currentGraphNodeEntry.Node))
				{
					state.IsAxisNode = 1;

					for (UEdGraphPin* pin : axisNode->Pins)
					{
						if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Axis)
						{
							FString DefaultString = pin->GetDefaultAsString();

							FVector2D Value;
							Value.InitFromString(DefaultString);

							state.InputActions.Add(pin->PinName, FInputActionState({}, Value.X, Value.Y));
						}
						else if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_2DAxis)
						{
							FString DefaultString = pin->GetDefaultAsString();

							FVector Value;
							Value.InitFromString(DefaultString);

							double xRad = FMath::DegreesToRadians(Value.X);
							double yRad = FMath::DegreesToRadians(Value.Y);

							double startAngleRad = FMath::Min(xRad, yRad);
							double endAngleRad = FMath::Max(xRad, yRad);

							// Full round
							if (endAngleRad - startAngleRad > DOUBLE_TWO_PI)
							{
								startAngleRad = -DOUBLE_HALF_PI;
								endAngleRad = DOUBLE_HALF_PI * 3;
							}
							else
							{
								while (startAngleRad < -DOUBLE_HALF_PI)
								{
									startAngleRad += DOUBLE_TWO_PI;
									endAngleRad += DOUBLE_TWO_PI;
								}
							}

							FString lhs;
							FString rhs;
							if (pin->PinName.ToString().Split(separator, &lhs, &rhs))
							{
								state.InputActions.Add(pin->PinName, FInputActionState({}, startAngleRad, endAngleRad, Value.Z, lhs, rhs));
							}
						}
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("!!!! %s"), *currentGraphNodeEntry.Node->GetClass()->GetName())
			}

			TArray<UEdGraphNode*> linkedNodes;
			GetNextNodes(currentGraphNodeEntry.Node, linkedNodes);

			for (UEdGraphNode* linkedNode : linkedNodes)
			{
				linkedNodesMapping[emplacedIndex].Guids.Add(linkedNode->NodeGuid);
				graphNodesQueue.Enqueue(FNodesQueueEntry(linkedNode, currentGraphNodeEntry.DepthIndex + 1, currentGraphNodeEntry.FirstLayerParentIndex > 0 ? currentGraphNodeEntry.FirstLayerParentIndex : emplacedIndex, pressedActions));
			}
		}

		for (size_t i = 0; i < inputSequenceAsset->States.Num(); i++)
		{
			if (linkedNodesMapping.IsValidIndex(i))
			{
				for (FGuid& linkedGuid : linkedNodesMapping[i].Guids)
				{
					if (indexMapping.Contains(linkedGuid))
					{
						inputSequenceAsset->States[i].NextIndice.Add(indexMapping[linkedGuid]);
					}
				}
			}
		}
	}
}



#pragma region UInputSequenceGraphSchema
#define LOCTEXT_NAMESPACE "UInputSequenceGraphSchema"

UEdGraphNode* FInputSequenceGraphSchemaAction_NewComment::PerformAction(class UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2D Location, bool bSelectNewNode/* = true*/)
{
	// Add menu item for creating comment boxes
	UEdGraphNode_Comment* CommentTemplate = NewObject<UEdGraphNode_Comment>();

	FVector2D SpawnLocation = Location;

	CommentTemplate->SetBounds(SelectedNodesBounds);
	SpawnLocation.X = CommentTemplate->NodePosX;
	SpawnLocation.Y = CommentTemplate->NodePosY;

	return FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate<UEdGraphNode_Comment>(ParentGraph, CommentTemplate, SpawnLocation);
}


UEdGraphNode* FInputSequenceGraphSchemaAction_NewNode::PerformAction(class UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2D Location, bool bSelectNewNode)
{
	UEdGraphNode* ResultNode = NULL;

	// If there is a template, we actually use it
	if (NodeTemplate != NULL)
	{
		const FScopedTransaction Transaction(LOCTEXT("K2_AddNode", "Add Node"));
		ParentGraph->Modify();
		if (FromPin)
		{
			FromPin->Modify();
		}

		// set outer to be the graph so it doesn't go away
		NodeTemplate->Rename(NULL, ParentGraph);
		ParentGraph->AddNode(NodeTemplate, true, bSelectNewNode);

		NodeTemplate->CreateNewGuid();
		NodeTemplate->PostPlacedNewNode();
		NodeTemplate->AllocateDefaultPins();
		NodeTemplate->AutowireNewNode(FromPin);

		NodeTemplate->NodePosX = Location.X;
		NodeTemplate->NodePosY = Location.Y;
		NodeTemplate->SnapToGrid(GetDefault<UEditorStyleSettings>()->GridSnapSize);

		ResultNode = NodeTemplate;

		ResultNode->SetFlags(RF_Transactional);
	}

	return ResultNode;
}

void FInputSequenceGraphSchemaAction_NewNode::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEdGraphSchemaAction::AddReferencedObjects(Collector);

	Collector.AddReferencedObject(NodeTemplate);
}

UEdGraphNode* FInputSequenceGraphSchemaAction_AddPin::PerformAction(class UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2D Location, bool bSelectNewNode)
{
	UEdGraphNode* ResultNode = NULL;

	// If there is a template, we actually use it
	if (InputName != NAME_None)
	{
		const int32 execPinCount = 2;

		const FScopedTransaction Transaction(LOCTEXT("K2_AddPin", "Add Pin"));

		UEdGraphNode::FCreatePinParams params;
		params.Index = CorrectedInputIndex + execPinCount;
		
		const FName& pc = IsAxis ? (Is2DAxis ? UInputSequenceGraphSchema::PC_2DAxis : UInputSequenceGraphSchema::PC_Axis) : UInputSequenceGraphSchema::PC_Action;

		AddPin(FromPin->GetOwningNode(), pc, InputName, params, InputAction);
	}

	return ResultNode;
}

const FName UInputSequenceGraphSchema::PC_Exec = FName("UInputSequenceGraphSchema_PC_Exec");

const FName UInputSequenceGraphSchema::PC_Action = FName("UInputSequenceGraphSchema_PC_Action");

const FName UInputSequenceGraphSchema::PC_Add = FName("UInputSequenceGraphSchema_PC_Add");

const FName UInputSequenceGraphSchema::PC_2DAxis = FName("UInputSequenceGraphSchema_PC_2DAxis");

const FName UInputSequenceGraphSchema::PC_Axis = FName("UInputSequenceGraphSchema_PC_Axis");

const FName UInputSequenceGraphSchema::PC_HubAdd = FName("UInputSequenceGraphSchema_PC_HubAdd");

void UInputSequenceGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	{
		// Add Axis node
		TSharedPtr<FInputSequenceGraphSchemaAction_NewNode> Action = AddNewActionAs<FInputSequenceGraphSchemaAction_NewNode>(ContextMenuBuilder, FText::GetEmpty(), LOCTEXT("AddNode_Axis", "Add Axis node..."), LOCTEXT("AddNode_Axis_Tooltip", "A new Axis node"));
		Action->NodeTemplate = NewObject<UInputSequenceGraphNode_Axis>(ContextMenuBuilder.OwnerOfTemporaries);
	}

	{
		// Add Press node
		TSharedPtr<FInputSequenceGraphSchemaAction_NewNode> Action = AddNewActionAs<FInputSequenceGraphSchemaAction_NewNode>(ContextMenuBuilder, FText::GetEmpty(), LOCTEXT("AddNode_Press", "Add Press node..."), LOCTEXT("AddNode_Press_Tooltip", "A new Press node"));
		Action->NodeTemplate = NewObject<UInputSequenceGraphNode_Press>(ContextMenuBuilder.OwnerOfTemporaries);
	}

	if (!ContextMenuBuilder.FromPin || ContextMenuBuilder.FromPin->Direction == EGPD_Output)
	{
		{
			// Add Go To Start node
			TSharedPtr<FInputSequenceGraphSchemaAction_NewNode> Action = AddNewActionAs<FInputSequenceGraphSchemaAction_NewNode>(ContextMenuBuilder, FText::GetEmpty(), LOCTEXT("AddNode_GoToStart", "Add Go To Start node..."), LOCTEXT("AddNode_GoToStart_Tooltip", "A new Go To Start node"));
			Action->NodeTemplate = NewObject<UInputSequenceGraphNode_GoToStart>(ContextMenuBuilder.OwnerOfTemporaries);
		}
	}

	{
		// Add Hub node
		TSharedPtr<FInputSequenceGraphSchemaAction_NewNode> Action = AddNewActionAs<FInputSequenceGraphSchemaAction_NewNode>(ContextMenuBuilder, FText::GetEmpty(), LOCTEXT("AddNode_Hub", "Add Hub node..."), LOCTEXT("AddNode_Hub_Tooltip", "A new Hub node"));
		Action->NodeTemplate = NewObject<UInputSequenceGraphNode_Hub>(ContextMenuBuilder.OwnerOfTemporaries);
	}

	// Add Start node if absent
	AddNewActionIfHasNo<UInputSequenceGraphNode_Start>(ContextMenuBuilder, FText::GetEmpty(), LOCTEXT("AddNode_Start", "Add Start node..."), LOCTEXT("AddNode_Start_Tooltip", "Define Start node"));
}

const FPinConnectionResponse UInputSequenceGraphSchema::CanCreateConnection(const UEdGraphPin* pinA, const UEdGraphPin* pinB) const
{
	if (pinA == nullptr || pinB == nullptr) return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("Pin(s)Null", "One or Both of the pins was null"));

	if (pinA->GetOwningNode() == pinB->GetOwningNode()) return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("PinsOfSameNode", "Both pins are on the same node"));

	if (pinA->Direction == pinB->Direction) return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("PinsOfSameDirection", "Both pins have same direction (both input or both output)"));

	if (pinA->PinType.PinCategory != pinB->PinType.PinCategory) return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("PinsMismatched", "The pin types are mismatched (Flow pins should be connected to Flow pins, Input Action pins - to Input Action pins)"));

	return FPinConnectionResponse(CONNECT_RESPONSE_BREAK_OTHERS_AB, TEXT(""));
}

void UInputSequenceGraphSchema::BreakPinLinks(UEdGraphPin& TargetPin, bool bSendsNodeNotifcation) const
{
	if (TargetPin.PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec) UEdGraphSchema::BreakPinLinks(TargetPin, bSendsNodeNotifcation);
}

void UInputSequenceGraphSchema::CreateDefaultNodesForGraph(UEdGraph& Graph) const
{
	FGraphNodeCreator<UInputSequenceGraphNode_Start> startNodeCreator(Graph);
	UInputSequenceGraphNode_Start* startNode = startNodeCreator.CreateNode();
	startNode->NodePosX = -300;
	startNodeCreator.Finalize();
	SetNodeMetaData(startNode, FNodeMetadata::DefaultGraphNode);
}

TSharedPtr<FEdGraphSchemaAction> UInputSequenceGraphSchema::GetCreateCommentAction() const
{
	return TSharedPtr<FEdGraphSchemaAction>(static_cast<FEdGraphSchemaAction*>(new FInputSequenceGraphSchemaAction_NewComment));
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



class SInputSequenceParameterMenu : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_RetVal_OneParam(FText, FGetSectionTitle, int32);

	SLATE_BEGIN_ARGS(SInputSequenceParameterMenu) : _AutoExpandMenu(false) {}

	SLATE_ARGUMENT(bool, AutoExpandMenu)
		SLATE_EVENT(FGetSectionTitle, OnGetSectionTitle)
		SLATE_END_ARGS();

	void Construct(const FArguments& InArgs)
	{
		this->bAutoExpandMenu = InArgs._AutoExpandMenu;

		ChildSlot
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("Menu.Background")).Padding(5)
				[
					SNew(SBox)
					.MinDesiredWidth(300)
			.MaxDesiredHeight(700) // Set max desired height to prevent flickering bug for menu larger than screen
			[
				SAssignNew(GraphMenu, SGraphActionMenu)
				.OnCollectStaticSections(this, &SInputSequenceParameterMenu::OnCollectStaticSections)
				.OnGetSectionTitle(this, &SInputSequenceParameterMenu::OnGetSectionTitle)
				.OnCollectAllActions(this, &SInputSequenceParameterMenu::CollectAllActions)
			.OnActionSelected(this, &SInputSequenceParameterMenu::OnActionSelected)
			.SortItemsRecursively(false)
			.AlphaSortItems(false)
			.AutoExpandActionMenu(bAutoExpandMenu)
			.ShowFilterTextBox(true)
			////// TODO.OnCreateCustomRowExpander_Static(&SNiagaraParameterMenu::CreateCustomActionExpander)
			////// TODO.OnCreateWidgetForAction_Lambda([](const FCreateWidgetForActionData* InData) { return SNew(SNiagaraGraphActionWidget, InData); })
			]
				]
			];
	}

	TSharedPtr<SEditableTextBox> GetSearchBox() { return GraphMenu->GetFilterTextBox(); }

protected:

	virtual void OnCollectStaticSections(TArray<int32>& StaticSectionIDs) = 0;

	virtual FText OnGetSectionTitle(int32 InSectionID) = 0;

	virtual void CollectAllActions(FGraphActionListBuilderBase& OutAllActions) = 0;

	virtual void OnActionSelected(const TArray<TSharedPtr<FEdGraphSchemaAction>>& SelectedActions, ESelectInfo::Type InSelectionType) = 0;

private:

	bool bAutoExpandMenu;

	TSharedPtr<SGraphActionMenu> GraphMenu;
};

class SInputSequenceParameterMenu_Pin : public SInputSequenceParameterMenu
{
public:
	SLATE_BEGIN_ARGS(SInputSequenceParameterMenu_Pin)
		: _AutoExpandMenu(false)
	{}
	//~ Begin Required Args
	SLATE_ARGUMENT(UEdGraphNode*, Node)
		//~ End Required Args
		SLATE_ARGUMENT(bool, AutoExpandMenu)
		SLATE_END_ARGS();

	void Construct(const FArguments& InArgs)
	{
		this->Node = InArgs._Node;

		SInputSequenceParameterMenu::FArguments SuperArgs;
		SuperArgs._AutoExpandMenu = InArgs._AutoExpandMenu;
		SInputSequenceParameterMenu::Construct(SuperArgs);
	}

protected:

	virtual void OnCollectStaticSections(TArray<int32>& StaticSectionIDs) override
	{
		StaticSectionIDs.Add(1);

		const bool isAxis = Node && Node->IsA<UInputSequenceGraphNode_Axis>();

		if (isAxis)
		{
			StaticSectionIDs.Add(2);
		}
	}

	virtual FText OnGetSectionTitle(int32 InSectionID) override
	{
		const bool isAxis = Node && Node->IsA<UInputSequenceGraphNode_Axis>();

		if (isAxis)
		{
			if (InSectionID == 1) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Axis", "Axis");
			if (InSectionID == 2) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Axis2D", "Axis 2D");

			if (InSectionID == 3) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Enhanced_Axis", "Axis (Enhanced Input)");
			if (InSectionID == 4) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Enhanced_Axis2D", "Axis 2D (Enhanced Input)");
		}
		else
		{
			if (InSectionID == 1) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Action", "Action");
			if (InSectionID == 2) return NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Section_Action_Enhanced", "Action (Enhanced Input)");
		}

		return FText::GetEmpty();
	}

	void CollectAction(const FName& inputName, UInputAction* inputAction, TSet<int32>& alreadyAdded, int& mappingIndex, const FText& toolTip,  int32 sectionID, bool isAxis, bool is2DAxis, TArray<TSharedPtr<FEdGraphSchemaAction>>& schemaActions)
	{
		if (Node && Node->FindPin(inputName))
		{
			alreadyAdded.Add(mappingIndex);
		}
		else
		{
			TSharedPtr<FInputSequenceGraphSchemaAction_AddPin> schemaAction(
				new FInputSequenceGraphSchemaAction_AddPin(
					FText::GetEmpty()
					, FText::FromName(inputName)
					, toolTip
					, 0
					, sectionID
				)
			);

			schemaAction->InputName = inputName;
			schemaAction->InputAction = inputAction;
			schemaAction->InputIndex = mappingIndex;
			schemaAction->CorrectedInputIndex = 0;
			schemaAction->IsAxis = isAxis;
			schemaAction->Is2DAxis = is2DAxis;

			schemaActions.Add(schemaAction);
		}

		mappingIndex++;
	}

	const FText simpleFormat = NSLOCTEXT("SInputSequenceParameterMenu_Pin", "AddPin_Tooltip", "Add {0} for {1}");
	const FText complex2DFormat = NSLOCTEXT("SInputSequenceParameterMenu_Pin_2D_Complex", "AddPin_Tooltip", "Add Axis pin for 2D {0} ^ {1}");

	virtual void CollectAllActions(FGraphActionListBuilderBase& OutAllActions) override
	{
		TSet<FName> inputNamesSet;

		const bool isAxis = Node&& Node->IsA<UInputSequenceGraphNode_Axis>();

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

		FARFilter Filter;
		Filter.ClassPaths.Add(UInputAction::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetList;
		AssetRegistryModule.Get().GetAssets(Filter, AssetList);

		if (isAxis)
		{
			for (const FInputAxisKeyMapping& axisMapping : UInputSettings::GetInputSettings()->GetAxisMappings())
			{
				inputNamesSet.FindOrAdd(axisMapping.AxisName);
			}
		}
		else
		{
			for (const FInputActionKeyMapping& actionMapping : UInputSettings::GetInputSettings()->GetActionMappings())
			{
				inputNamesSet.FindOrAdd(actionMapping.ActionName);
			}
		}

		TSet<UInputAction*> enhInputActionsSet;
		TSet<FName> enhInputNamesSet2D; ////// TODO
		TSet<FName> enhInputNamesSet3D; ////// TODO

		for (const FAssetData& assetData : AssetList)
		{
			if (UInputAction* inputAction = Cast<UInputAction>(assetData.GetAsset()))
			{
				if (isAxis)
				{
					if (inputAction->ValueType == EInputActionValueType::Axis1D)
					{
						enhInputActionsSet.FindOrAdd(inputAction);
					}
					else if (inputAction->ValueType == EInputActionValueType::Axis2D)
					{
						enhInputNamesSet2D.FindOrAdd(inputAction->GetFName());
					}
					else if (inputAction->ValueType == EInputActionValueType::Axis3D)
					{
						enhInputNamesSet3D.FindOrAdd(inputAction->GetFName());
					}
				}
				else
				{
					if (inputAction->ValueType == EInputActionValueType::Boolean)
					{
						enhInputActionsSet.FindOrAdd(inputAction);
					}
				}
			}
		}

		int32 mappingIndex = 0;

		TSet<int32> alreadyAdded;

		TArray<TSharedPtr<FEdGraphSchemaAction>> schemaActions;

		// Classic Input
		for (const FName& inputName : inputNamesSet)
		{
			CollectAction(
				inputName
				, nullptr
				, alreadyAdded
				, mappingIndex
				, FText::Format(simpleFormat, FText::FromString(isAxis ? "Axis pin" : "Action pin"), FText::FromName(inputName))
				, 1
				, isAxis
				, false
				, schemaActions
			);
		}

		// Classic Input сomplex 2D
		if (isAxis)
		{
			for (const FName& inputNameA : inputNamesSet)
			{
				for (const FName& inputNameB : inputNamesSet)
				{
					if (inputNameA != inputNameB)
					{
						FName pairedName = FName(inputNameA.ToString().Append(separator).Append(inputNameB.ToString()));

						CollectAction(
							pairedName
							, nullptr
							, alreadyAdded
							, mappingIndex
							, FText::Format(complex2DFormat, FText::FromName(inputNameA), FText::FromName(inputNameB))
							, 2
							, true
							, true
							, schemaActions
						);
					}
				}
			}
		}

		// Enhanced Input
		for (UInputAction* enhInputAction : enhInputActionsSet)
		{
			CollectAction(
				enhInputAction->GetFName()
				, enhInputAction
				, alreadyAdded
				, mappingIndex
				, FText::Format(simpleFormat, FText::FromString(isAxis ? "Axis pin" : "Action pin"), FText::FromName(enhInputAction->GetFName()))
				, isAxis ? 3 : 2
				, isAxis
				, false
				, schemaActions
			);
		}

		for (TSharedPtr<FEdGraphSchemaAction> schemaAction : schemaActions)
		{
			TSharedPtr<FInputSequenceGraphSchemaAction_AddPin> addPinAction = StaticCastSharedPtr<FInputSequenceGraphSchemaAction_AddPin>(schemaAction);
			
			for (int32 alreadyAddedIndex : alreadyAdded)
			{
				if (alreadyAddedIndex < addPinAction->InputIndex) addPinAction->CorrectedInputIndex++;
			}

			OutAllActions.AddAction(schemaAction);
		}
	}

	virtual void OnActionSelected(const TArray<TSharedPtr<FEdGraphSchemaAction>>& SelectedActions, ESelectInfo::Type InSelectionType) override
	{
		if (InSelectionType == ESelectInfo::OnMouseClick || InSelectionType == ESelectInfo::OnKeyPress || SelectedActions.Num() == 0)
		{
			for (int32 ActionIndex = 0; ActionIndex < SelectedActions.Num(); ActionIndex++)
			{
				FSlateApplication::Get().DismissAllMenus();
				SelectedActions[ActionIndex]->PerformAction(Node->GetGraph(), Node->FindPin(NAME_None, EGPD_Input), FVector2D::ZeroVector);
			}
		}
	}

private:

	UEdGraphNode* Node;
};

void SInputSequenceGraphNode_Dynamic::Construct(const FArguments& InArgs, UEdGraphNode* InNode)
{
	SetCursor(EMouseCursor::CardinalCross);

	GraphNode = InNode;

	if (UInputSequenceGraphNode_Dynamic* pressNode = Cast<UInputSequenceGraphNode_Dynamic>(InNode))
	{
		pressNode->OnUpdateGraphNode.BindLambda([&]() { UpdateGraphNode(); });
	}

	UpdateGraphNode();
}

SInputSequenceGraphNode_Dynamic::~SInputSequenceGraphNode_Dynamic()
{
	if (UInputSequenceGraphNode_Dynamic* pressNode = Cast<UInputSequenceGraphNode_Dynamic>(GraphNode))
	{
		pressNode->OnUpdateGraphNode.Unbind();
	}
}



#pragma region UInputSequenceGraphNode_Base
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Base"

void UInputSequenceGraphNode_Base::AutowireNewNode(UEdGraphPin* FromPin)
{
	if (FromPin && FromPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec)
	{
		EEdGraphPinDirection targetDirection = EGPD_Output;
		if (FromPin->Direction == EGPD_Output) targetDirection = EGPD_Input;

		for (UEdGraphPin* Pin : Pins)
		{
			if (targetDirection == Pin->Direction)
			{
				GetSchema()->TryCreateConnection(FromPin, Pin);
				return;
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_GoToStart
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_GoToStart"

void UInputSequenceGraphNode_GoToStart::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UInputSequenceGraphSchema::PC_Exec, NAME_None);
}

FText UInputSequenceGraphNode_GoToStart::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("UInputSequenceGraphNode_GoToStart_Title", "Go To Start node");
}

FLinearColor UInputSequenceGraphNode_GoToStart::GetNodeTitleColor() const { return FLinearColor::Green; }

FText UInputSequenceGraphNode_GoToStart::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_GoToStart_ToolTip", "This is a Go To Start node of Input sequence...");
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Hub
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Hub"

void UInputSequenceGraphNode_Hub::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UInputSequenceGraphSchema::PC_Exec, NAME_None);
	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_Exec, FName("1"));

	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_HubAdd, "Add pin");
}

FText UInputSequenceGraphNode_Hub::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("UInputSequenceGraphNode_Hub_Title", "Hub node");
}

FLinearColor UInputSequenceGraphNode_Hub::GetNodeTitleColor() const { return FLinearColor::Green; }

FText UInputSequenceGraphNode_Hub::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_Hub_ToolTip", "This is a Hub node of Input sequence...");
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Input
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Input"

UInputSequenceGraphNode_Input::UInputSequenceGraphNode_Input(const FObjectInitializer& ObjectInitializer) :Super(ObjectInitializer)
{
	EditConditionIndex = 0;
	canBePassedAfterTime = 0;

	isOverridingRequirePreciseMatch = 0;
	requirePreciseMatch = 0;

	ResetAfterTime = 0.2f;

	isOverridingResetAfterTime = 0;
	isResetAfterTime = 0;

	StateObject = nullptr;
	StateContext = "";
}

void UInputSequenceGraphNode_Input::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UInputSequenceGraphSchema::PC_Exec, NAME_None);
	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_Exec, NAME_None);
}

FLinearColor UInputSequenceGraphNode_Input::GetNodeTitleColor() const { return FLinearColor::Blue; }

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Press
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Press"

UInputSequenceGraphNode_Press::UInputSequenceGraphNode_Press(const FObjectInitializer& ObjectInitializer) :Super(ObjectInitializer)
{
	EditConditionIndex = 1;
}

void UInputSequenceGraphNode_Press::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();

	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_Add, "Add pin");
}

void UInputSequenceGraphNode_Press::DestroyNode()
{
	for (UEdGraphPin* FromPin : Pins)
	{
		if (FromPin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action && FromPin->HasAnyConnections())
		{
			UEdGraphNode* linkedNode = FromPin->LinkedTo[0]->GetOwningNode();
			linkedNode->Modify();
			linkedNode->DestroyNode();
		}
	}

	Super::DestroyNode();
}

FText UInputSequenceGraphNode_Press::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("UInputSequenceGraphNode_GoToStart_Title", "Press node");
}

FText UInputSequenceGraphNode_Press::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_Press_ToolTip", "This is a Press node of Input sequence...");
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Release
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Release"

UInputSequenceGraphNode_Release::UInputSequenceGraphNode_Release(const FObjectInitializer& ObjectInitializer) :Super(ObjectInitializer)
{
	EditConditionIndex = 2;
	PassedAfterTime = 3;
}

void UInputSequenceGraphNode_Release::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UInputSequenceGraphNode_Release, canBePassedAfterTime))
	{
		OnUpdateGraphNode.ExecuteIfBound();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

FText UInputSequenceGraphNode_Release::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return canBePassedAfterTime
		? FText::Format(LOCTEXT("UInputSequenceGraphNode_Release_TitleWithDelay", "Release node [{0}]"), FText::FromString(FString::SanitizeFloat(PassedAfterTime, 1)))
		: LOCTEXT("UInputSequenceGraphNode_Release_Title", "Release node");
}

FText UInputSequenceGraphNode_Release::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_Release_ToolTip", "This is a Release node of Input sequence...");
}

void UInputSequenceGraphNode_Release::AutowireNewNode(UEdGraphPin* FromPin)
{
	if (FromPin->Direction == EGPD_Output && FromPin && FromPin->PinType.PinCategory != UInputSequenceGraphSchema::PC_Exec)
	{
		UEdGraphPin* OtherPin = CreatePin(EGPD_Input, UInputSequenceGraphSchema::PC_Action, FromPin->PinName);
		GetSchema()->TryCreateConnection(FromPin, OtherPin);
	}
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Start
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Start"

void UInputSequenceGraphNode_Start::AllocateDefaultPins()
{
	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_Exec, NAME_None);
}

FText UInputSequenceGraphNode_Start::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("UInputSequenceGraphNode_Start_Title", "Start node");
}

FLinearColor UInputSequenceGraphNode_Start::GetNodeTitleColor() const { return FLinearColor::Red; }

FText UInputSequenceGraphNode_Start::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_Start_ToolTip", "This is a Start node of Input sequence...");
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region UInputSequenceGraphNode_Axis
#define LOCTEXT_NAMESPACE "UInputSequenceGraphNode_Axis"

void UInputSequenceGraphNode_Axis::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();

	CreatePin(EGPD_Output, UInputSequenceGraphSchema::PC_Add, "Add pin");
}

FText UInputSequenceGraphNode_Axis::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("UInputSequenceGraphNode_Axis_Title", "Axis node");
}

FText UInputSequenceGraphNode_Axis::GetTooltipText() const
{
	return LOCTEXT("UInputSequenceGraphNode_Axis_ToolTip", "This is an Axis node of Input sequence...");
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SToolTip_Mock
#define LOCTEXT_NAMESPACE "SToolTip_Mock"

class SToolTip_Mock : public SLeafWidget, public IToolTip
{
public:

	SLATE_BEGIN_ARGS(SToolTip_Mock) {}
	SLATE_END_ARGS()

		void Construct(const FArguments& InArgs) {}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override { return LayerId; }
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

	virtual TSharedRef<class SWidget> AsWidget() { return SNullWidget::NullWidget; }
	virtual TSharedRef<SWidget> GetContentWidget() { return SNullWidget::NullWidget; }
	virtual void SetContentWidget(const TSharedRef<SWidget>& InContentWidget) override {}
	virtual bool IsEmpty() const override { return false; }
	virtual bool IsInteractive() const { return false; }
	virtual void OnOpening() override {}
	virtual void OnClosed() override {}
};

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region S1DAxisTextBox
#define LOCTEXT_NAMESPACE "S1DAxisTextBox"

//Class implementation to create 2 editable text boxes to represent vector2D graph pin
class S1DAxisTextBox : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(S1DAxisTextBox) {}
	SLATE_ATTRIBUTE(FString, VisibleText_X)
		SLATE_ATTRIBUTE(FString, VisibleText_Y)
		SLATE_EVENT(FOnFloatValueCommitted, OnFloatCommitted_Box_X)
		SLATE_EVENT(FOnFloatValueCommitted, OnFloatCommitted_Box_Y)
		SLATE_END_ARGS()

		//Construct editable text boxes with the appropriate getter & setter functions along with tool tip text
		void Construct(const FArguments& InArgs)
	{
		VisibleText_X = InArgs._VisibleText_X;
		VisibleText_Y = InArgs._VisibleText_Y;
		const FLinearColor LabelClr = FLinearColor(1.f, 1.f, 1.f, 0.4f);

		this->ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
			.AutoWidth().Padding(2).HAlign(HAlign_Fill)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
			.Text(LOCTEXT("LeftParenthesis", "("))
			.ColorAndOpacity(LabelClr)
			]

		+ SHorizontalBox::Slot()
			.AutoWidth().Padding(2).HAlign(HAlign_Fill)
			[
				//Create Text box 0 
				SNew(SNumericEntryBox<float>)
				.Value(this, &S1DAxisTextBox::GetTypeInValue_X)
			.OnValueCommitted(InArgs._OnFloatCommitted_Box_X)
			.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
			.UndeterminedString(LOCTEXT("MultipleValues", "Multiple Values"))
			.ToolTipText(LOCTEXT("VectorNodeXAxisValueLabel_ToolTip", "From value"))
			.EditableTextBoxStyle(&FAppStyle::GetWidgetStyle<FEditableTextBoxStyle>("Graph.VectorEditableTextBox"))
			.BorderForegroundColor(FLinearColor::White)
			.BorderBackgroundColor(FLinearColor::White)
			]

		+ SHorizontalBox::Slot()
			.AutoWidth().Padding(2).HAlign(HAlign_Fill)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
			.Text(LOCTEXT("Mediator", ","))
			.ColorAndOpacity(LabelClr)
			]

		+ SHorizontalBox::Slot()
			.AutoWidth().Padding(2).HAlign(HAlign_Fill)
			[
				//Create Text box 1
				SNew(SNumericEntryBox<float>)
				.Value(this, &S1DAxisTextBox::GetTypeInValue_Y)
			.OnValueCommitted(InArgs._OnFloatCommitted_Box_Y)
			.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
			.UndeterminedString(LOCTEXT("MultipleValues", "Multiple Values"))
			.ToolTipText(LOCTEXT("VectorNodeYAxisValueLabel_ToolTip", "To value"))
			.EditableTextBoxStyle(&FAppStyle::GetWidgetStyle<FEditableTextBoxStyle>("Graph.VectorEditableTextBox"))
			.BorderForegroundColor(FLinearColor::White)
			.BorderBackgroundColor(FLinearColor::White)
			]

		+ SHorizontalBox::Slot()
			.AutoWidth().Padding(2).HAlign(HAlign_Fill)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
			.Text(LOCTEXT("RightParenthesis", ")"))
			.ColorAndOpacity(LabelClr)
			]
			]
			];
	}

private:

	//Get value for X text box
	TOptional<float> GetTypeInValue_X() const { return FCString::Atof(*(VisibleText_X.Get())); }

	//Get value for Y text box
	TOptional<float> GetTypeInValue_Y() const { return FCString::Atof(*(VisibleText_Y.Get())); }

	TAttribute<FString> VisibleText_X;
	TAttribute<FString> VisibleText_Y;
};

FString MakeVector2DString(const FString& X, const FString& Y)
{
	return FString(TEXT("(X=")) + X + FString(TEXT(",Y=")) + Y + FString(TEXT(")"));
}

FString MakeVectorString(const FString& X, const FString& Y, const FString& Z)
{
	return FString(TEXT("(X=")) + X + FString(TEXT(",Y=")) + Y + FString(TEXT(",Z=")) + Z + FString(TEXT(")"));
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SStickZone
#define LOCTEXT_NAMESPACE "SStickZone"

class SStickZone : public SLeafWidget
{
public:

	DECLARE_DELEGATE_TwoParams(FOnValueChanged, float, SGraphPin_2DAxis::ETextBoxIndex);

	SLATE_BEGIN_ARGS(SStickZone) {}
	SLATE_EVENT(FOnValueChanged, OnValueChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnValueChanged = InArgs._OnValueChanged;
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		FVector2D localSize = AllottedGeometry.GetLocalSize();

		FVector2D center = localSize / 2;

		TArray<FVector2D> LinePoints;

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(center.X, 0.f));
		LinePoints.Add(FVector2D(center.X, localSize.Y));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(0, center.Y));
		LinePoints.Add(FVector2D(localSize.X, center.Y));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(0, 0));
		LinePoints.Add(FVector2D(localSize.X, 0));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(0, localSize.Y));
		LinePoints.Add(FVector2D(localSize.X, localSize.Y));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(0, 0));
		LinePoints.Add(FVector2D(0, localSize.Y));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		++LayerId;
		LinePoints.Empty();
		LinePoints.Add(FVector2D(localSize.X, 0));
		LinePoints.Add(FVector2D(localSize.X, localSize.Y));

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LinePoints,
			ESlateDrawEffect::None,
			FLinearColor::Red
		);

		int num = 1;
		double deltaAngleRad = AngleRadRange.Y - AngleRadRange.X;

		double deltaAnglePath = FMath::Abs(deltaAngleRad);
		double stepAngleThreshold = FMath::DegreesToRadians(4);
		while (deltaAnglePath > stepAngleThreshold * num) { num++; }

		double stepAngleRad = deltaAngleRad / num;
		double currentAngleRad = AngleRadRange.X;

		FVector2D dir;
		FVector2D prevDir;

		FLinearColor color = FLinearColor::White;
		color.A = 0.75;

		++LayerId;
		for (size_t i = 0; i < num; i++)
		{
			dir.X = center.X * FMath::Cos(currentAngleRad);
			dir.Y = center.Y * FMath::Sin(-(currentAngleRad));

			LinePoints.Empty();
			LinePoints.Add(FVector2D(center + dir * Scale));
			LinePoints.Add(FVector2D(center + dir * 10));

			FSlateDrawElement::MakeLines(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(),
				LinePoints,
				ESlateDrawEffect::None,
				color
			);

			if (i > 0)
			{
				prevDir.X = center.X * FMath::Cos(currentAngleRad - stepAngleRad);
				prevDir.Y = center.Y * FMath::Sin(-(currentAngleRad - stepAngleRad));

				LinePoints.Empty();
				LinePoints.Add(FVector2D(center + dir * Scale));
				LinePoints.Add(FVector2D(center + prevDir * Scale));

				FSlateDrawElement::MakeLines(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(),
					LinePoints,
					ESlateDrawEffect::None,
					color
				);
			}

			currentAngleRad += stepAngleRad;
		}

		return LayerId;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			FVector2D localSize = MyGeometry.GetLocalSize();
			FVector2D center = localSize / 2;
			FVector2D localPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

			FVector2D position = (localPosition - center) / center;
			position.Y = -position.Y;
			
			double angleRad = FMath::Atan(position.Y / position.X);
			if (position.X < 0) angleRad += DOUBLE_PI;

			prevAngleRad = angleRad;

			AngleRadRange.X = angleRad;
			OnValueChanged.ExecuteIfBound(FMath::RoundToDouble(FMath::RadiansToDegrees(angleRad)), SGraphPin_2DAxis::ETextBoxIndex::TextBox_X);
			
			AngleRadRange.Y = angleRad;
			OnValueChanged.ExecuteIfBound(FMath::RoundToDouble(FMath::RadiansToDegrees(angleRad)), SGraphPin_2DAxis::ETextBoxIndex::TextBox_Y);

			Scale = FMath::RoundToDouble(100 * position.Size()) / 100;
			OnValueChanged.ExecuteIfBound(Scale, SGraphPin_2DAxis::ETextBoxIndex::TextBox_Z);

			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		else
		{
			return FReply::Unhandled();
		}
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if ((MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton) && this->HasMouseCapture())
		{
			return FReply::Handled().ReleaseMouseCapture();
		}
		else
		{
			return FReply::Unhandled();
		}
	}

	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (this->HasMouseCapture())
		{
			if (IsHovered())
			{
				FVector2D localSize = MyGeometry.GetLocalSize();
				FVector2D center = localSize / 2;
				FVector2D localPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

				FVector2D position = (localPosition - center) / center;
				position.Y = -position.Y;

				double angleRad = FMath::Atan(position.Y / position.X);
				if (position.X < 0) angleRad += DOUBLE_PI;

				double deltaAngleRad = angleRad - prevAngleRad;

				if (deltaAngleRad > DOUBLE_PI) deltaAngleRad -= DOUBLE_TWO_PI;
				
				if (deltaAngleRad < -DOUBLE_PI) deltaAngleRad += DOUBLE_TWO_PI;

				AngleRadRange.Y += deltaAngleRad;
				OnValueChanged.ExecuteIfBound(FMath::RoundToDouble(FMath::RadiansToDegrees(AngleRadRange.Y)), SGraphPin_2DAxis::ETextBoxIndex::TextBox_Y);

				prevAngleRad = angleRad;

				Scale = FMath::RoundToDouble(100 * position.Size()) / 100;
				OnValueChanged.ExecuteIfBound(Scale, SGraphPin_2DAxis::ETextBoxIndex::TextBox_Z);
			}

			return FReply::Handled();
		}

		return FReply::Unhandled();
	}

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{		
		Scale = FMath::Max(0, Scale + (MouseEvent.GetWheelDelta() > 0 ? 0.01 : -0.01));

		OnValueChanged.ExecuteIfBound(Scale, SGraphPin_2DAxis::TextBox_Z);

		return FReply::Handled();
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

	FVector2D AngleRadRange;
	
	double prevAngleRad;

	double Scale;

	FOnValueChanged OnValueChanged;
};

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_2DAxis
#define LOCTEXT_NAMESPACE "SGraphPin_2DAxis"

void SGraphPin_2DAxis::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Default);
	this->SetToolTipText(LOCTEXT("ToolTip", "Mock ToolTip"));

	SetVisibility(MakeAttributeSP(this, &SGraphPin_2DAxis::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	// Create the pin icon widget
	TSharedRef<SWidget> SelfPinWidgetRef = SPinTypeSelector::ConstructPinTypeImage(
		MakeAttributeSP(this, &SGraphPin_2DAxis::GetPinIcon),
		MakeAttributeSP(this, &SGraphPin_2DAxis::GetPinColor),
		MakeAttributeSP(this, &SGraphPin_2DAxis::GetSecondaryPinIcon),
		MakeAttributeSP(this, &SGraphPin_2DAxis::GetSecondaryPinColor));

	SelfPinWidgetRef->SetVisibility(EVisibility::Hidden);

	TSharedRef<SWidget> PinWidgetRef = SelfPinWidgetRef;

	PinImage = PinWidgetRef;

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
		.Visibility(this, &SGraphPin_2DAxis::GetPinStatusIconVisibility)
		.ContentPadding(0)
		.OnClicked(this, &SGraphPin_2DAxis::ClickedOnPinStatusIcon)
		[
			SNew(SImage).Image(this, &SGraphPin_2DAxis::GetPinStatusIcon)
		];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);

	LabelWidget->SetToolTipText(MakeAttributeRaw(this, &SGraphPin_2DAxis::ToolTipText_Raw_Label));

	// Create the widget used for the pin body (status indicator, label, and value)

	LabelAndValue = SNew(SWrapBox).PreferredSize(150.f);

	LabelAndValue->AddSlot().VAlign(VAlign_Center)[LabelWidget];

	ValueWidget = GetDefaultValueWidget();

	if (ValueWidget != SNullWidget::NullWidget)
	{
		TSharedPtr<SBox> ValueBox;
		LabelAndValue->AddSlot()
			.Padding(FMargin(InArgs._SideToSideMargin, 0, 0, 0))
			.VAlign(VAlign_Center)
			[
				SAssignNew(ValueBox, SBox).Padding(0.0f)
				[
					ValueWidget.ToSharedRef()
				]
			];

		if (!DoesWidgetHandleSettingEditingEnabled())
		{
			ValueBox->SetEnabled(TAttribute<bool>(this, &SGraphPin::IsEditingEnabled));
		}
	}

	LabelAndValue->AddSlot().VAlign(VAlign_Center)[PinStatusIndicator];

	TSharedPtr<SHorizontalBox> PinContent;

	FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, InArgs._SideToSideMargin, 0)
		[
			SNew(SButton).ToolTipText_Raw(this, &SGraphPin_2DAxis::ToolTipText_Raw_RemovePin)
			.Cursor(EMouseCursor::Hand)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.ForegroundColor(FSlateColor::UseForeground())
		.OnClicked_Raw(this, &SGraphPin_2DAxis::OnClicked_Raw_RemovePin)
		[
			SNew(SImage).Image(FAppStyle::GetBrush("Cross"))
		]
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LabelAndValue.ToSharedRef()
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(InArgs._SideToSideMargin, 0, 0, 0)
		[
			PinWidgetRef
		];

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(this, &SGraphPin_2DAxis::GetPinColor)
		[
			SNew(SLevelOfDetailBranchNode)
			.UseLowDetailSlot(this, &SGraphPin_2DAxis::UseLowDetailPinNames)
		.LowDetail()
		[
			//@TODO: Try creating a pin-colored line replacement that doesn't measure text / call delegates but still renders
			PinWidgetRef
		]
	.HighDetail()
		[
			PinContent.ToSharedRef()
		]
		]
	);

	SetToolTip(SNew(SToolTip_Mock));

	if (StickZone.IsValid())
	{
		FString DefaultString = GraphPinObj->GetDefaultAsString();

		FVector Value;
		Value.InitFromString(DefaultString);

		StickZone->AngleRadRange.X = FMath::DegreesToRadians(Value.X);
		StickZone->AngleRadRange.Y = FMath::DegreesToRadians(Value.Y);

		StickZone->Scale = Value.Z;
	}
}

SGraphPin_2DAxis::~SGraphPin_2DAxis()
{
	StickZone.Reset();
}

FSlateColor SGraphPin_2DAxis::GetPinTextColor() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	FString lhs;
	FString rhs;
	GraphPin->PinName.ToString().Split(separator, &lhs, &rhs);

	if (!UInputSettings::GetInputSettings()->DoesAxisExist(FName(lhs))) return FLinearColor::Red;

	if (!UInputSettings::GetInputSettings()->DoesAxisExist(FName(rhs))) return FLinearColor::Red;

	if (GraphPin)

		// If there is no schema there is no owning node (or basically this is a deleted node)
		if (UEdGraphNode* GraphNode = GraphPin ? GraphPin->GetOwningNodeUnchecked() : nullptr)
		{
			const bool bDisabled = (!GraphNode->IsNodeEnabled() || GraphNode->IsDisplayAsDisabledForced() || !IsEditingEnabled() || GraphNode->IsNodeUnrelated());
			if (GraphPin->bOrphanedPin)
			{
				FLinearColor PinColor = FLinearColor::Red;
				if (bDisabled)
				{
					PinColor.A = .25f;
				}
				return PinColor;
			}
			else if (bDisabled)
			{
				return FLinearColor(1.0f, 1.0f, 1.0f, 0.5f);
			}
			if (bUsePinColorForText)
			{
				return GetPinColor();
			}
		}

	return FLinearColor::White;
}

TSharedRef<SWidget> SGraphPin_2DAxis::GetDefaultValueWidget()
{
	//Create widget

	const FLinearColor LabelClr = FLinearColor(1.f, 1.f, 1.f, 0.4f);

	TSharedPtr<SBorder> StickCoordsBorder = nullptr;

	TSharedPtr<SWidget> resultWidget = SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().Padding(4).VAlign(VAlign_Center)
		[
			SNew(SGridPanel)
			.FillColumn(0,0).FillColumn(1, 1).FillColumn(2, 0)
			.FillRow(0, 0).FillRow(1, 1).FillRow(2, 0)

			+ SGridPanel::Slot(1, 0)
			[
				SNew(SBox).WidthOverride(16).HeightOverride(16).VAlign(VAlign_Center).HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("StandardDialog.SmallFont"))
					.Text(LOCTEXT("LeftBottomPoint", "+y"))
					.ColorAndOpacity(LabelClr)
				]
			]

			+ SGridPanel::Slot(2, 1)
			[
				SNew(SBox).WidthOverride(16).HeightOverride(16).VAlign(VAlign_Center).HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("StandardDialog.SmallFont"))
					.Text(LOCTEXT("LeftBottomPoint", "+x"))
					.ColorAndOpacity(LabelClr)
				]
			]
			
			+ SGridPanel::Slot(1,1)
			[
				SNew(SBox).WidthOverride(64).HeightOverride(64).Clipping(EWidgetClipping::ClipToBounds)
				[
					SAssignNew(StickZone, SStickZone)
					.OnValueChanged(this, &SGraphPin_2DAxis::OnStickZoneValueChanged)
				]
			]

			+ SGridPanel::Slot(1,2)
			[
				SNew(SBox).WidthOverride(16).HeightOverride(16).VAlign(VAlign_Center).HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("StandardDialog.SmallFont"))
					.Text(LOCTEXT("RightTopPoint", "-y"))
					.ColorAndOpacity(LabelClr)
				]
			]

			+ SGridPanel::Slot(0, 1)
			[
				SNew(SBox).WidthOverride(16).HeightOverride(16).VAlign(VAlign_Center).HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("StandardDialog.SmallFont"))
					.Text(LOCTEXT("RightTopPoint", "-x"))
					.ColorAndOpacity(LabelClr)
				]
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().Padding(4).VAlign(VAlign_Center)
		[
			SNew(SGridPanel).FillColumn(0, 1).FillColumn(1, 1).FillRow(0, 1).FillRow(1, 1)

			+ SGridPanel::Slot(0, 0).ColumnSpan(2).Padding(2)
			[
				SNew(S1DAxisTextBox)
				.VisibleText_X(this, &SGraphPin_2DAxis::GetCurrentValue_X)
				.VisibleText_Y(this, &SGraphPin_2DAxis::GetCurrentValue_Y)
				.IsEnabled(this, &SGraphPin_2DAxis::GetDefaultValueIsEditable)
				.OnFloatCommitted_Box_X(this, &SGraphPin_2DAxis::OnChangedValueTextBox_X)
				.OnFloatCommitted_Box_Y(this, &SGraphPin_2DAxis::OnChangedValueTextBox_Y)
			]

			+ SGridPanel::Slot(0, 1).VAlign(VAlign_Center).Padding(2)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
				.Text(LOCTEXT("LeftParenthesis", "min:"))
				.ColorAndOpacity(LabelClr)
			]

			+ SGridPanel::Slot(1, 1).VAlign(VAlign_Center).Padding(2)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SGraphPin_2DAxis::GetTypeInValue_Z)
				.OnValueCommitted(this, &SGraphPin_2DAxis::OnChangedValueTextBox_Z)
				.Font(FAppStyle::GetFontStyle( "Graph.VectorEditableTextBox" ))
				.UndeterminedString(LOCTEXT("MultipleValues", "Multiple Values"))
				.EditableTextBoxStyle(&FAppStyle::GetWidgetStyle<FEditableTextBoxStyle>("Graph.VectorEditableTextBox"))
				.BorderForegroundColor(FLinearColor::White)
				.BorderBackgroundColor(FLinearColor::White)
			]
		];

		return resultWidget.ToSharedRef();
}

FString SGraphPin_2DAxis::GetCurrentValue_X() const { return GetValue(TextBox_X); }

FString SGraphPin_2DAxis::GetCurrentValue_Y() const { return GetValue(TextBox_Y); }

TOptional<float> SGraphPin_2DAxis::GetTypeInValue_Z() const { return FCString::Atof(*(GetValue(TextBox_Z))); }

FString SGraphPin_2DAxis::GetValue(ETextBoxIndex Index) const
{
	FString DefaultString = GraphPinObj->GetDefaultAsString();

	FVector Value;
	Value.InitFromString(DefaultString);

	if (Index == TextBox_X)
	{
		return FString::Printf(TEXT("%f"), Value.X);
	}
	else if (Index == TextBox_Y)
	{
		return FString::Printf(TEXT("%f"), Value.Y);
	}
	else
	{
		return FString::Printf(TEXT("%f"), Value.Z);
	}
}

void SGraphPin_2DAxis::OnChangedValueTextBox_X(float NewValue, ETextCommit::Type CommitInfo)
{
	if (GraphPinObj->IsPendingKill())
	{
		return;
	}

	if (StickZone.IsValid()) StickZone->AngleRadRange.X = FMath::DegreesToRadians(NewValue);

	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);

	TrySetDefaultValue(MakeVectorString(ValueStr, GetValue(TextBox_Y), GetValue(TextBox_Z)));
}

void SGraphPin_2DAxis::OnChangedValueTextBox_Y(float NewValue, ETextCommit::Type CommitInfo)
{
	if (GraphPinObj->IsPendingKill())
	{
		return;
	}

	if (StickZone.IsValid()) StickZone->AngleRadRange.Y = FMath::DegreesToRadians(NewValue);

	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);

	TrySetDefaultValue(MakeVectorString(GetValue(TextBox_X), ValueStr, GetValue(TextBox_Z)));
}

void SGraphPin_2DAxis::OnChangedValueTextBox_Z(float NewValue, ETextCommit::Type CommitInfo)
{
	if (GraphPinObj->IsPendingKill())
	{
		return;
	}

	if (StickZone.IsValid())
	{
		StickZone->Scale = NewValue;
	}

	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);

	TrySetDefaultValue(MakeVectorString(GetValue(TextBox_X), GetValue(TextBox_Y), ValueStr));
}

FText SGraphPin_2DAxis::ToolTipText_Raw_Label() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	FString lhs;
	FString rhs;
	GraphPin->PinName.ToString().Split(separator, &lhs, &rhs);

	return UInputSettings::GetInputSettings()->DoesAxisExist(FName(lhs)) && UInputSettings::GetInputSettings()->DoesAxisExist(FName(rhs))
		? FText::GetEmpty()
		: LOCTEXT("Label_TootTip_Error", "Cant find corresponding Axis name in Input Settings!");
}

FText SGraphPin_2DAxis::ToolTipText_Raw_RemovePin() const { return LOCTEXT("RemovePin_Tooltip", "Click to remove Axis pin"); }

FReply SGraphPin_2DAxis::OnClicked_Raw_RemovePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		UEdGraphNode* FromNode = FromPin->GetOwningNode();

		UEdGraph* ParentGraph = FromNode->GetGraph();

		if (FromPin->HasAnyConnections())
		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeleteNode", "Delete Node"));

			ParentGraph->Modify();

			UEdGraphNode* linkedGraphNode = FromPin->LinkedTo[0]->GetOwningNode();

			linkedGraphNode->Modify();
			linkedGraphNode->DestroyNode();
		}

		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeletePin", "Delete Pin"));

			FromNode->RemovePin(FromPin);

			FromNode->Modify();

			if (UInputSequenceGraphNode_Dynamic* dynNode = Cast<UInputSequenceGraphNode_Dynamic>(FromNode)) dynNode->OnUpdateGraphNode.ExecuteIfBound();
		}
	}

	return FReply::Handled();
}

void SGraphPin_2DAxis::EvalAndSetValueFromMouseEvent(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	FVector2D localPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

	FVector2D localSize = MyGeometry.GetLocalSize();

	FVector2D newValue = (2 * localPosition - localSize) / localSize;

	newValue.X = FMath::RoundToFloat(newValue.X * 100) / 100;

	newValue.Y = -FMath::RoundToFloat(newValue.Y * 100) / 100;

	const FString ValueXStr = FString::Printf(TEXT("%f"), newValue.X);

	const FString ValueYStr = FString::Printf(TEXT("%f"), newValue.Y);

	TrySetDefaultValue(MakeVectorString(ValueXStr, ValueYStr, GetValue(TextBox_Z)));
}

void SGraphPin_2DAxis::TrySetDefaultValue(const FString& VectorString)
{
	if (GraphPinObj->GetDefaultAsString() != VectorString)
	{
		const FScopedTransaction Transaction(LOCTEXT("ChangeVectorPinValue", "Change Vector Pin Value"));
		GraphPinObj->Modify();

		//Set new default value
		GraphPinObj->GetSchema()->TrySetDefaultValue(*GraphPinObj, VectorString);


	}
}

void SGraphPin_2DAxis::OnStickZoneValueChanged(float NewValue, ETextBoxIndex Index)
{
	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);

	if (Index == ETextBoxIndex::TextBox_X)
	{
		TrySetDefaultValue(MakeVectorString(ValueStr, GetValue(TextBox_Y), GetValue(TextBox_Z)));
	}
	else if (Index == ETextBoxIndex::TextBox_Y)
	{
		TrySetDefaultValue(MakeVectorString(GetValue(TextBox_X), ValueStr, GetValue(TextBox_Z)));
	}
	else
	{
		TrySetDefaultValue(MakeVectorString(GetValue(TextBox_X), GetValue(TextBox_Y), ValueStr));
	}
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_Action
#define LOCTEXT_NAMESPACE "SGraphPin_Action"

void SGraphPin_Action::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Default);
	this->SetToolTipText(LOCTEXT("ToolTip", "Mock ToolTip"));

	SetVisibility(MakeAttributeSP(this, &SGraphPin_Action::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	const bool bIsInput = (GetDirection() == EGPD_Input);

	// Create the pin icon widget
	TSharedRef<SWidget> SelfPinWidgetRef = SPinTypeSelector::ConstructPinTypeImage(
		MakeAttributeSP(this, &SGraphPin_Action::GetPinIcon),
		MakeAttributeSP(this, &SGraphPin_Action::GetPinColor),
		MakeAttributeSP(this, &SGraphPin_Action::GetSecondaryPinIcon),
		MakeAttributeSP(this, &SGraphPin_Action::GetSecondaryPinColor));

	SelfPinWidgetRef->SetVisibility(MakeAttributeRaw(this, &SGraphPin_Action::Visibility_Raw_SelfPin));

	TSharedRef<SWidget> PinWidgetRef =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.CircleArrowUp"))
			.Visibility_Raw(this, &SGraphPin_Action::Visibility_Raw_ArrowUp)
		]
	+ SOverlay::Slot().VAlign(VAlign_Center).HAlign(HAlign_Center)
		[
			SelfPinWidgetRef
		];

	PinImage = PinWidgetRef;

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
		.Visibility(this, &SGraphPin_Action::GetPinStatusIconVisibility)
		.ContentPadding(0)
		.OnClicked(this, &SGraphPin_Action::ClickedOnPinStatusIcon)
		[
			SNew(SImage)
			.Image(this, &SGraphPin_Action::GetPinStatusIcon)
		];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);
	LabelWidget->SetToolTipText(MakeAttributeRaw(this, &SGraphPin_Action::ToolTipText_Raw_Label));

	// Create the widget used for the pin body (status indicator, label, and value)
	LabelAndValue =
		SNew(SWrapBox)
		.PreferredSize(150.f);

	if (!bIsInput) // Output pin
	{
		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				PinStatusIndicator
			];

		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				LabelWidget
			];
	}
	else // Input pin
	{
		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				LabelWidget
			];

		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				PinStatusIndicator
			];
	}

	TSharedPtr<SHorizontalBox> PinContent;
	if (bIsInput) // Input pin
	{
		FullPinHorizontalRowWidget = PinContent =
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				PinWidgetRef
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.CircleArrowUp"))
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				LabelAndValue.ToSharedRef()
			];
	}
	else // Output pin
	{
		FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				SNew(SButton).ToolTipText_Raw(this, &SGraphPin_Action::ToolTipText_Raw_RemovePin)
				.Cursor(EMouseCursor::Hand)
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ForegroundColor(FSlateColor::UseForeground())
			.OnClicked_Raw(this, &SGraphPin_Action::OnClicked_Raw_RemovePin)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Cross"))
			]
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				LabelAndValue.ToSharedRef()
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				SNew(SButton).ToolTipText_Raw(this, &SGraphPin_Action::ToolTipText_Raw_TogglePin)
				.Cursor(EMouseCursor::Hand)
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ForegroundColor(FSlateColor::UseForeground())
			.OnClicked_Raw(this, &SGraphPin_Action::OnClicked_Raw_TogglePin)
			[
				SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.CircleArrowDown"))
			]
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				PinWidgetRef
			];
	}

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(this, &SGraphPin_Action::GetPinColor)
		[
			SNew(SLevelOfDetailBranchNode)
			.UseLowDetailSlot(this, &SGraphPin_Action::UseLowDetailPinNames)
		.LowDetail()
		[
			//@TODO: Try creating a pin-colored line replacement that doesn't measure text / call delegates but still renders
			PinWidgetRef
		]
	.HighDetail()
		[
			PinContent.ToSharedRef()
		]
		]
	);

	SetToolTip(SNew(SToolTip_Mock));
}

bool IsValidEnhancedInputPin(UEdGraphPin* GraphPin)
{
	if (UInputSequenceGraphNode_Input* inputNode = Cast<UInputSequenceGraphNode_Input>(GraphPin->GetOwningNode()))
	{
		if (inputNode->GetPinsInputActions().Contains(GraphPin->PinName))
		{
			return inputNode->GetPinsInputActions()[GraphPin->PinName].operator bool();
		}
	}

	return false;
}

FSlateColor SGraphPin_Action::GetPinTextColor() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	if (!UInputSettings::GetInputSettings()->DoesActionExist(GraphPin->PinName) && !IsValidEnhancedInputPin(GraphPin)) return FLinearColor::Red;

	if (GraphPin)

		// If there is no schema there is no owning node (or basically this is a deleted node)
		if (UEdGraphNode* GraphNode = GraphPin ? GraphPin->GetOwningNodeUnchecked() : nullptr)
		{
			const bool bDisabled = (!GraphNode->IsNodeEnabled() || GraphNode->IsDisplayAsDisabledForced() || !IsEditingEnabled() || GraphNode->IsNodeUnrelated());
			if (GraphPin->bOrphanedPin)
			{
				FLinearColor PinColor = FLinearColor::Red;
				if (bDisabled)
				{
					PinColor.A = .25f;
				}
				return PinColor;
			}
			else if (bDisabled)
			{
				return FLinearColor(1.0f, 1.0f, 1.0f, 0.5f);
			}
			if (bUsePinColorForText)
			{
				return GetPinColor();
			}
		}

	return FLinearColor::White;
}

FText SGraphPin_Action::ToolTipText_Raw_Label() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	return (UInputSettings::GetInputSettings()->DoesActionExist(GraphPin->PinName) || IsValidEnhancedInputPin(GraphPin))
		? FText::GetEmpty()
		: LOCTEXT("Label_TootTip_Error", "Cant find corresponding Action name in Input Settings or InputAction (Enhanced Input) in Content!");
}

EVisibility SGraphPin_Action::Visibility_Raw_SelfPin() const
{
	if (UEdGraphPin* pin = GetPinObj())
	{
		return pin->HasAnyConnections() ? EVisibility::Visible : EVisibility::Hidden;
	}

	return EVisibility::Hidden;
}

EVisibility SGraphPin_Action::Visibility_Raw_ArrowUp() const
{
	if (UEdGraphPin* pin = GetPinObj())
	{
		return pin->HasAnyConnections() ? EVisibility::Hidden : EVisibility::Visible;
	}

	return EVisibility::Visible;
}

FText SGraphPin_Action::ToolTipText_Raw_RemovePin() const { return LOCTEXT("RemovePin_Tooltip", "Click to remove Action pin"); }

FReply SGraphPin_Action::OnClicked_Raw_RemovePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		UEdGraphNode* FromNode = FromPin->GetOwningNode();

		UEdGraph* ParentGraph = FromNode->GetGraph();

		if (FromPin->HasAnyConnections())
		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeleteNode", "Delete Node"));

			ParentGraph->Modify();

			UEdGraphNode* linkedGraphNode = FromPin->LinkedTo[0]->GetOwningNode();

			linkedGraphNode->Modify();
			linkedGraphNode->DestroyNode();
		}

		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeletePin", "Delete Pin"));

			FromNode->RemovePin(FromPin);

			FromNode->Modify();

			if (UInputSequenceGraphNode_Input* inputNode = Cast<UInputSequenceGraphNode_Input>(FromNode))
			{
				inputNode->GetPinsInputActions().Remove(FromPin->PinName);
			}

			if (UInputSequenceGraphNode_Dynamic* dynNode = Cast<UInputSequenceGraphNode_Dynamic>(FromNode))
			{
				dynNode->OnUpdateGraphNode.ExecuteIfBound();
			}
		}
	}

	return FReply::Handled();
}

FText SGraphPin_Action::ToolTipText_Raw_TogglePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		return FromPin->HasAnyConnections()
			? LOCTEXT("RemovePin_Tooltip_Click", "Click to set CLICK mode")
			: LOCTEXT("RemovePin_Tooltip_Press", "Click to set PRESS mode");
	}

	return LOCTEXT("RemovePin_Tooltip_Error", "Invalid pin object!");
}

FReply SGraphPin_Action::OnClicked_Raw_TogglePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		UEdGraphNode* FromNode = FromPin->GetOwningNode();

		UEdGraph* ParentGraph = FromNode->GetGraph();

		if (FromPin->HasAnyConnections())
		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeleteNode", "Delete Node"));

			ParentGraph->Modify();

			UEdGraphNode* linkedGraphNode = FromPin->LinkedTo[0]->GetOwningNode();

			linkedGraphNode->Modify();
			linkedGraphNode->DestroyNode();
		}
		else
		{
			const FScopedTransaction Transaction(LOCTEXT("K2_AddNode", "Add Node"));

			ParentGraph->Modify();

			if (FromPin) FromPin->Modify();

			// set outer to be the graph so it doesn't go away
			UInputSequenceGraphNode_Release* ResultNode = NewObject<UInputSequenceGraphNode_Release>(ParentGraph);
			ParentGraph->AddNode(ResultNode, true, false);

			ResultNode->CreateNewGuid();
			ResultNode->PostPlacedNewNode();
			ResultNode->AllocateDefaultPins();
			ResultNode->AutowireNewNode(FromPin);

			ResultNode->NodePosX = FromNode->NodePosX + 300;
			ResultNode->NodePosY = FromNode->NodePosY;

			ResultNode->SnapToGrid(GetDefault<UEditorStyleSettings>()->GridSnapSize);;

			ResultNode->SetFlags(RF_Transactional);

			////// TODO Maybe cast FromNode in declaration line

			if (UInputSequenceGraphNode_Input* fromInputNode = Cast<UInputSequenceGraphNode_Input>(FromNode))
			{
				if (fromInputNode->GetPinsInputActions().Contains(FromPin->PinName))
				{
					ResultNode->GetPinsInputActions().Add(FromPin->PinName, fromInputNode->GetPinsInputActions()[FromPin->PinName]);
				}
			}
		}
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_Add
#define LOCTEXT_NAMESPACE "SGraphPin_Add"

void SGraphPin_Add::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Hand);
	this->SetToolTipText(LOCTEXT("AddPin_ToolTip", "Click to add new pin"));

	SetVisibility(MakeAttributeSP(this, &SGraphPin_Add::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	TSharedRef<SWidget> PinWidgetRef = SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.PlusCircle"));

	PinImage = PinWidgetRef;

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
		.Visibility(this, &SGraphPin_Add::GetPinStatusIconVisibility)
		.ContentPadding(0)
		.OnClicked(this, &SGraphPin_Add::ClickedOnPinStatusIcon)
		[
			SNew(SImage).Image(this, &SGraphPin_Add::GetPinStatusIcon)
		];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);

	// Create the widget used for the pin body (status indicator, label, and value)
	LabelAndValue =
		SNew(SWrapBox)
		.PreferredSize(150.f);

	LabelAndValue->AddSlot()
		.VAlign(VAlign_Center)
		[
			LabelWidget
		];

	LabelAndValue->AddSlot()
		.VAlign(VAlign_Center)
		[
			PinStatusIndicator
		];

	TSharedPtr<SHorizontalBox> PinContent;
	FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, InArgs._SideToSideMargin, 0)
		[
			LabelAndValue.ToSharedRef()
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			PinWidgetRef
		];

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(this, &SGraphPin_Add::GetPinColor)
		[
			SAssignNew(AddButton, SComboButton)
			.HasDownArrow(false)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.ForegroundColor(FSlateColor::UseForeground())
		.OnGetMenuContent(this, &SGraphPin_Add::OnGetAddButtonMenuContent)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ButtonContent()
		[
			PinContent.ToSharedRef()
		]
		]
	);
}

TSharedRef<SWidget> SGraphPin_Add::OnGetAddButtonMenuContent()
{
	TSharedRef<SInputSequenceParameterMenu_Pin> MenuWidget = SNew(SInputSequenceParameterMenu_Pin).Node(GetPinObj()->GetOwningNode());

	AddButton->SetMenuContentWidgetToFocus(MenuWidget->GetSearchBox());

	return MenuWidget;
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_Axis
#define LOCTEXT_NAMESPACE "SGraphPin_Axis"

void SGraphPin_Axis::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Default);
	this->SetToolTipText(LOCTEXT("ToolTip", "Mock ToolTip"));

	SetVisibility(MakeAttributeSP(this, &SGraphPin_Axis::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	// Create the pin icon widget
	TSharedRef<SWidget> SelfPinWidgetRef = SPinTypeSelector::ConstructPinTypeImage(
		MakeAttributeSP(this, &SGraphPin_Axis::GetPinIcon),
		MakeAttributeSP(this, &SGraphPin_Axis::GetPinColor),
		MakeAttributeSP(this, &SGraphPin_Axis::GetSecondaryPinIcon),
		MakeAttributeSP(this, &SGraphPin_Axis::GetSecondaryPinColor));

	SelfPinWidgetRef->SetVisibility(EVisibility::Hidden);

	TSharedRef<SWidget> PinWidgetRef = SelfPinWidgetRef;

	PinImage = PinWidgetRef;

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
			.Visibility(this, &SGraphPin_Axis::GetPinStatusIconVisibility)
			.ContentPadding(0)
			.OnClicked(this, &SGraphPin_Axis::ClickedOnPinStatusIcon)
			[
				SNew(SImage).Image(this, &SGraphPin_Axis::GetPinStatusIcon)
			];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);
	
	LabelWidget->SetToolTipText(MakeAttributeRaw(this, &SGraphPin_Axis::ToolTipText_Raw_Label));

	// Create the widget used for the pin body (status indicator, label, and value)

	LabelAndValue = SNew(SWrapBox).PreferredSize(150.f);

	LabelAndValue->AddSlot().VAlign(VAlign_Center)[LabelWidget];

	ValueWidget = GetDefaultValueWidget();

	if (ValueWidget != SNullWidget::NullWidget)
	{
		TSharedPtr<SBox> ValueBox;
		LabelAndValue->AddSlot()
			.Padding(FMargin(InArgs._SideToSideMargin, 0, 0, 0))
			.VAlign(VAlign_Center)
			[
				SAssignNew(ValueBox, SBox).Padding(0.0f)
					[
						ValueWidget.ToSharedRef()
					]
			];

		if (!DoesWidgetHandleSettingEditingEnabled())
		{
			ValueBox->SetEnabled(TAttribute<bool>(this, &SGraphPin::IsEditingEnabled));
		}
	}

	LabelAndValue->AddSlot().VAlign(VAlign_Center)[PinStatusIndicator];

	TSharedPtr<SHorizontalBox> PinContent;

	FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, InArgs._SideToSideMargin, 0)
		[
			SNew(SButton).ToolTipText_Raw(this, &SGraphPin_Axis::ToolTipText_Raw_RemovePin)
				.Cursor(EMouseCursor::Hand)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.ForegroundColor(FSlateColor::UseForeground())
				.OnClicked_Raw(this, &SGraphPin_Axis::OnClicked_Raw_RemovePin)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Cross"))
				]
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LabelAndValue.ToSharedRef()
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(InArgs._SideToSideMargin, 0, 0, 0)
		[
			PinWidgetRef
		];

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(this, &SGraphPin_Axis::GetPinColor)
		[
			SNew(SLevelOfDetailBranchNode)
				.UseLowDetailSlot(this, &SGraphPin_Axis::UseLowDetailPinNames)
				.LowDetail()
				[
					//@TODO: Try creating a pin-colored line replacement that doesn't measure text / call delegates but still renders
					PinWidgetRef
				]
				.HighDetail()
				[
					PinContent.ToSharedRef()
				]
		]
	);

	SetToolTip(SNew(SToolTip_Mock));
}

FSlateColor SGraphPin_Axis::GetPinTextColor() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	if (!UInputSettings::GetInputSettings()->DoesAxisExist(GraphPin->PinName) && !IsValidEnhancedInputPin(GraphPin)) return FLinearColor::Red;

	if (GraphPin)

		// If there is no schema there is no owning node (or basically this is a deleted node)
		if (UEdGraphNode* GraphNode = GraphPin ? GraphPin->GetOwningNodeUnchecked() : nullptr)
		{
			const bool bDisabled = (!GraphNode->IsNodeEnabled() || GraphNode->IsDisplayAsDisabledForced() || !IsEditingEnabled() || GraphNode->IsNodeUnrelated());
			if (GraphPin->bOrphanedPin)
			{
				FLinearColor PinColor = FLinearColor::Red;
				if (bDisabled)
				{
					PinColor.A = .25f;
				}
				return PinColor;
			}
			else if (bDisabled)
			{
				return FLinearColor(1.0f, 1.0f, 1.0f, 0.5f);
			}
			if (bUsePinColorForText)
			{
				return GetPinColor();
			}
		}

	return FLinearColor::White;
}

TSharedRef<SWidget> SGraphPin_Axis::GetDefaultValueWidget()
{
	//Create widget
	return SNew(S1DAxisTextBox)
		.VisibleText_X(this, &SGraphPin_Axis::GetCurrentValue_X)
		.VisibleText_Y(this, &SGraphPin_Axis::GetCurrentValue_Y)
		.IsEnabled(this, &SGraphPin_Axis::GetDefaultValueIsEditable)
		.OnFloatCommitted_Box_X(this, &SGraphPin_Axis::OnChangedValueTextBox_X)
		.OnFloatCommitted_Box_Y(this, &SGraphPin_Axis::OnChangedValueTextBox_Y);
}

FString SGraphPin_Axis::GetCurrentValue_X() const { return GetValue(TextBox_X); }

FString SGraphPin_Axis::GetCurrentValue_Y() const { return GetValue(TextBox_Y); }

FString SGraphPin_Axis::GetValue(ETextBoxIndex Index) const
{
	FString DefaultString = GraphPinObj->GetDefaultAsString();

	FVector2D Value;
	Value.InitFromString(DefaultString);

	if (Index == TextBox_X)
	{
		return FString::Printf(TEXT("%f"), Value.X);
	}
	else
	{
		return FString::Printf(TEXT("%f"), Value.Y);
	}
}

void SGraphPin_Axis::OnChangedValueTextBox_X(float NewValue, ETextCommit::Type CommitInfo)
{
	if (GraphPinObj->IsPendingKill())
	{
		return;
	}

	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);
	const FString Vector2DString = MakeVector2DString(ValueStr, GetValue(TextBox_Y));

	if (GraphPinObj->GetDefaultAsString() != Vector2DString)
	{
		const FScopedTransaction Transaction(LOCTEXT("ChangeVectorPinValue", "Change Vector Pin Value"));
		GraphPinObj->Modify();

		//Set new default value
		GraphPinObj->GetSchema()->TrySetDefaultValue(*GraphPinObj, Vector2DString);
	}
}

void SGraphPin_Axis::OnChangedValueTextBox_Y(float NewValue, ETextCommit::Type CommitInfo)
{
	if (GraphPinObj->IsPendingKill())
	{
		return;
	}

	const FString ValueStr = FString::Printf(TEXT("%f"), NewValue);
	const FString Vector2DString = MakeVector2DString(GetValue(TextBox_X), ValueStr);

	if (GraphPinObj->GetDefaultAsString() != Vector2DString)
	{
		const FScopedTransaction Transaction(LOCTEXT("ChangeVectorPinValue", "Change Vector Pin Value"));
		GraphPinObj->Modify();

		//Set new default value
		GraphPinObj->GetSchema()->TrySetDefaultValue(*GraphPinObj, Vector2DString);
	}
}

FText SGraphPin_Axis::ToolTipText_Raw_Label() const
{
	UEdGraphPin* GraphPin = GetPinObj();

	return (UInputSettings::GetInputSettings()->DoesAxisExist(GraphPin->PinName) || IsValidEnhancedInputPin(GraphPin))
		? FText::GetEmpty()
		: LOCTEXT("Label_TootTip_Error", "Cant find corresponding Axis name in Input Settings or InputAction (Enhanced Input) in Content!");
}

FText SGraphPin_Axis::ToolTipText_Raw_RemovePin() const { return LOCTEXT("RemovePin_Tooltip", "Click to remove Axis pin"); }

FReply SGraphPin_Axis::OnClicked_Raw_RemovePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		UEdGraphNode* FromNode = FromPin->GetOwningNode();

		UEdGraph* ParentGraph = FromNode->GetGraph();

		if (FromPin->HasAnyConnections())
		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeleteNode", "Delete Node"));

			ParentGraph->Modify();

			UEdGraphNode* linkedGraphNode = FromPin->LinkedTo[0]->GetOwningNode();

			linkedGraphNode->Modify();
			linkedGraphNode->DestroyNode();
		}

		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeletePin", "Delete Pin"));

			FromNode->RemovePin(FromPin);

			FromNode->Modify();

			if (UInputSequenceGraphNode_Dynamic* dynNode = Cast<UInputSequenceGraphNode_Dynamic>(FromNode)) dynNode->OnUpdateGraphNode.ExecuteIfBound();
		}
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_HubAdd
#define LOCTEXT_NAMESPACE "SGraphPin_HubAdd"

void SGraphPin_HubAdd::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Hand);
	this->SetToolTipText(LOCTEXT("AddPin_ToolTip", "Click to add new pin"));

	SetVisibility(MakeAttributeSP(this, &SGraphPin_HubAdd::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	TSharedRef<SWidget> PinWidgetRef = SNew(SImage).Image(FAppStyle::Get().GetBrush("Icons.PlusCircle"));

	PinImage = PinWidgetRef;

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
		.Visibility(this, &SGraphPin_HubAdd::GetPinStatusIconVisibility)
		.ContentPadding(0)
		.OnClicked(this, &SGraphPin_HubAdd::ClickedOnPinStatusIcon)
		[
			SNew(SImage).Image(this, &SGraphPin_HubAdd::GetPinStatusIcon)
		];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);

	// Create the widget used for the pin body (status indicator, label, and value)
	LabelAndValue =
		SNew(SWrapBox)
		.PreferredSize(150.f);

	LabelAndValue->AddSlot()
		.VAlign(VAlign_Center)
		[
			LabelWidget
		];

	LabelAndValue->AddSlot()
		.VAlign(VAlign_Center)
		[
			PinStatusIndicator
		];

	TSharedPtr<SHorizontalBox> PinContent;
	FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, InArgs._SideToSideMargin, 0)
		[
			LabelAndValue.ToSharedRef()
		]
	+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			PinWidgetRef
		];

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(this, &SGraphPin_HubAdd::GetPinColor)
		[
			SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.ForegroundColor(FSlateColor::UseForeground())
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.OnClicked_Raw(this, &SGraphPin_HubAdd::OnClicked_Raw)
		[
			PinContent.ToSharedRef()
		]
		]
	);
}

FReply SGraphPin_HubAdd::OnClicked_Raw()
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		const FScopedTransaction Transaction(LOCTEXT("K2_AddPin", "Add Pin"));

		int32 outputPinsCount = 0;
		for (UEdGraphPin* pin : FromPin->GetOwningNode()->Pins)
		{
			if (pin->Direction == EGPD_Output) outputPinsCount++;
		}

		UEdGraphNode::FCreatePinParams params;
		params.Index = outputPinsCount;

		AddPin(FromPin->GetOwningNode(), UInputSequenceGraphSchema::PC_Exec, FName(FString::FromInt(outputPinsCount)), params, nullptr);
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region SGraphPin_HubExec
#define LOCTEXT_NAMESPACE "SGraphPin_HubExec"

void SGraphPin_HubExec::Construct(const FArguments& Args, UEdGraphPin* InPin)
{
	SGraphPin::FArguments InArgs = SGraphPin::FArguments();

	bUsePinColorForText = InArgs._UsePinColorForText;
	this->SetCursor(EMouseCursor::Default);

	SetVisibility(MakeAttributeSP(this, &SGraphPin_HubExec::GetPinVisiblity));

	GraphPinObj = InPin;
	check(GraphPinObj != NULL);

	const UEdGraphSchema* Schema = GraphPinObj->GetSchema();
	checkf(
		Schema,
		TEXT("Missing schema for pin: %s with outer: %s of type %s"),
		*(GraphPinObj->GetName()),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetName()) : TEXT("NULL OUTER"),
		GraphPinObj->GetOuter() ? *(GraphPinObj->GetOuter()->GetClass()->GetName()) : TEXT("NULL OUTER")
	);

	const bool bIsInput = (GetDirection() == EGPD_Input);

	// Create the pin icon widget
	TSharedRef<SWidget> PinWidgetRef = SPinTypeSelector::ConstructPinTypeImage(
		MakeAttributeSP(this, &SGraphPin_HubExec::GetPinIcon),
		MakeAttributeSP(this, &SGraphPin_HubExec::GetPinColor),
		MakeAttributeSP(this, &SGraphPin_HubExec::GetSecondaryPinIcon),
		MakeAttributeSP(this, &SGraphPin_HubExec::GetSecondaryPinColor));
	PinImage = PinWidgetRef;

	PinWidgetRef->SetCursor(
		TAttribute<TOptional<EMouseCursor::Type> >::Create(
			TAttribute<TOptional<EMouseCursor::Type> >::FGetter::CreateRaw(this, &SGraphPin_HubExec::GetPinCursor)
		)
	);

	// Create the pin indicator widget (used for watched values)
	static const FName NAME_NoBorder("NoBorder");
	TSharedRef<SWidget> PinStatusIndicator =
		SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), NAME_NoBorder)
		.Visibility(this, &SGraphPin_HubExec::GetPinStatusIconVisibility)
		.ContentPadding(0)
		.OnClicked(this, &SGraphPin_HubExec::ClickedOnPinStatusIcon)
		[
			SNew(SImage)
			.Image(this, &SGraphPin_HubExec::GetPinStatusIcon)
		];

	TSharedRef<SWidget> LabelWidget = GetLabelWidget(InArgs._PinLabelStyle);

	// Create the widget used for the pin body (status indicator, label, and value)
	LabelAndValue =
		SNew(SWrapBox)
		.PreferredSize(150.f);

	if (!bIsInput)
	{
		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				PinStatusIndicator
			];

		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				LabelWidget
			];
	}
	else
	{
		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				LabelWidget
			];

		ValueWidget = GetDefaultValueWidget();

		if (ValueWidget != SNullWidget::NullWidget)
		{
			TSharedPtr<SBox> ValueBox;
			LabelAndValue->AddSlot()
				.Padding(bIsInput ? FMargin(InArgs._SideToSideMargin, 0, 0, 0) : FMargin(0, 0, InArgs._SideToSideMargin, 0))
				.VAlign(VAlign_Center)
				[
					SAssignNew(ValueBox, SBox)
					.Padding(0.0f)
				[
					ValueWidget.ToSharedRef()
				]
				];

			if (!DoesWidgetHandleSettingEditingEnabled())
			{
				ValueBox->SetEnabled(TAttribute<bool>(this, &SGraphPin_HubExec::IsEditingEnabled));
			}
		}

		LabelAndValue->AddSlot()
			.VAlign(VAlign_Center)
			[
				PinStatusIndicator
			];
	}

	TSharedPtr<SHorizontalBox> PinContent;
	if (bIsInput) // Input pin
	{
		FullPinHorizontalRowWidget = PinContent =
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				PinWidgetRef
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				LabelAndValue.ToSharedRef()
			];
	}
	else // Output pin
	{
		FullPinHorizontalRowWidget = PinContent = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				SNew(SButton).ToolTipText_Raw(this, &SGraphPin_HubExec::ToolTipText_Raw_RemovePin)
				.Cursor(EMouseCursor::Hand)
			.ButtonStyle(FAppStyle::Get(), "NoBorder")
			.ForegroundColor(FSlateColor::UseForeground())
			.OnClicked_Raw(this, &SGraphPin_HubExec::OnClicked_Raw_RemovePin)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Cross"))
			]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, InArgs._SideToSideMargin, 0)
			[
				LabelAndValue.ToSharedRef()
			]
		+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				PinWidgetRef
			];
	}

	// Set up a hover for pins that is tinted the color of the pin.
	SBorder::Construct(SBorder::FArguments()
		.BorderImage(this, &SGraphPin_HubExec::GetPinBorder)
		.BorderBackgroundColor(this, &SGraphPin_HubExec::GetPinColor)
		.OnMouseButtonDown(this, &SGraphPin_HubExec::OnPinNameMouseDown)
		[
			SNew(SLevelOfDetailBranchNode)
			.UseLowDetailSlot(this, &SGraphPin_HubExec::UseLowDetailPinNames)
		.LowDetail()
		[
			//@TODO: Try creating a pin-colored line replacement that doesn't measure text / call delegates but still renders
			PinWidgetRef
		]
	.HighDetail()
		[
			PinContent.ToSharedRef()
		]
		]
	);

	TSharedPtr<IToolTip> TooltipWidget = SNew(SToolTip)
		.Text(this, &SGraphPin_HubExec::GetTooltipText);

	SetToolTip(TooltipWidget);

	CachePinIcons();
}

FText SGraphPin_HubExec::ToolTipText_Raw_RemovePin() const { return LOCTEXT("RemoveHubPin_Tooltip", "Click to remove Hub pin"); }

FReply SGraphPin_HubExec::OnClicked_Raw_RemovePin() const
{
	if (UEdGraphPin* FromPin = GetPinObj())
	{
		UEdGraphNode* FromNode = FromPin->GetOwningNode();

		UEdGraph* ParentGraph = FromNode->GetGraph();

		{
			const FScopedTransaction Transaction(LOCTEXT("K2_DeletePin", "Delete Pin"));

			int nextAfterRemovedIndex = FromNode->Pins.IndexOfByKey(FromPin) + 1;

			if (FromNode->Pins.IsValidIndex(nextAfterRemovedIndex))
			{
				for (size_t i = nextAfterRemovedIndex; i < FromNode->Pins.Num(); i++)
				{
					UEdGraphPin* pin = FromNode->Pins[i];

					if (pin->Direction == EGPD_Output && pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Exec)
					{
						pin->PinName = FName(FString::FromInt(i - 1));
					}
				}
			}

			FromNode->RemovePin(FromPin);

			FromNode->Modify();

			if (UInputSequenceGraphNode_Dynamic* dynNode = Cast<UInputSequenceGraphNode_Dynamic>(FromNode)) dynNode->OnUpdateGraphNode.ExecuteIfBound();
		}
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
#pragma endregion



#pragma region FInputSequenceAssetEditor
#define LOCTEXT_NAMESPACE "FInputSequenceAssetEditor"

const FName FInputSequenceAssetEditor::AppIdentifier(TEXT("FInputSequenceAssetEditor_AppIdentifier"));
const FName FInputSequenceAssetEditor::DetailsTabId(TEXT("FInputSequenceAssetEditor_DetailsTab_Id"));
const FName FInputSequenceAssetEditor::GraphTabId(TEXT("FInputSequenceAssetEditor_GraphTab_Id"));

void FInputSequenceAssetEditor::InitInputSequenceAssetEditor(const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UInputSequenceAsset* inputSequenceAsset)
{
	check(inputSequenceAsset != NULL);

	InputSequenceAsset = inputSequenceAsset;

	InputSequenceAsset->SetFlags(RF_Transactional);

	TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("FInputSequenceAssetEditor_StandaloneDefaultLayout")
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.1f)
				->SetHideTabWell(true)
			)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.3f)
					->AddTab(DetailsTabId, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.7f)
					->AddTab(GraphTabId, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
			)
		);

	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, AppIdentifier, StandaloneDefaultLayout, true, true, InputSequenceAsset);
}

void FInputSequenceAssetEditor::RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenuCategory", "Input Sequence Asset Editor"));
	TSharedRef<FWorkspaceItem> WorkspaceMenuCategoryRef = WorkspaceMenuCategory.ToSharedRef();

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FInputSequenceAssetEditor::SpawnTab_DetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab_DisplayName", "Details"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

	InTabManager->RegisterTabSpawner(GraphTabId, FOnSpawnTab::CreateSP(this, &FInputSequenceAssetEditor::SpawnTab_GraphTab))
		.SetDisplayName(LOCTEXT("GraphTab_DisplayName", "Graph"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.EventGraph_16x"));
}

void FInputSequenceAssetEditor::UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(GraphTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

TSharedRef<SDockTab> FInputSequenceAssetEditor::SpawnTab_DetailsTab(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == DetailsTabId);

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs = FDetailsViewArgs();
	DetailsViewArgs.bUpdatesFromSelection = false;
	DetailsViewArgs.bLockable = false;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(InputSequenceAsset);

	return SNew(SDockTab).Label(LOCTEXT("DetailsTab_Label", "Details"))[DetailsView.ToSharedRef()];
}

TSharedRef<SDockTab> FInputSequenceAssetEditor::SpawnTab_GraphTab(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId().TabType == GraphTabId);

	check(InputSequenceAsset != NULL);

	if (InputSequenceAsset->EdGraph == NULL)
	{
		InputSequenceAsset->EdGraph = NewObject<UInputSequenceGraph>(InputSequenceAsset, NAME_None, RF_Transactional);
		InputSequenceAsset->EdGraph->GetSchema()->CreateDefaultNodesForGraph(*InputSequenceAsset->EdGraph);
	}

	check(InputSequenceAsset->EdGraph != NULL);

	FGraphAppearanceInfo AppearanceInfo;
	AppearanceInfo.CornerText = LOCTEXT("GraphTab_AppearanceInfo_CornerText", "Input Sequence Asset");

	SGraphEditor::FGraphEditorEvents InEvents;
	InEvents.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(this, &FInputSequenceAssetEditor::OnSelectionChanged);
	InEvents.OnTextCommitted = FOnNodeTextCommitted::CreateSP(this, &FInputSequenceAssetEditor::OnNodeTitleCommitted);

	CreateCommandList();

	return SNew(SDockTab)
		.Label(LOCTEXT("GraphTab_Label", "Graph"))
		.TabColorScale(GetTabColorScale())
		[
			SAssignNew(GraphEditorPtr, SGraphEditor)
			.AdditionalCommands(GraphEditorCommands)
		.Appearance(AppearanceInfo)
		.GraphEvents(InEvents)
		.TitleBar(SNew(STextBlock).Text(LOCTEXT("GraphTab_Title", "Input Sequence Asset")).TextStyle(FAppStyle::Get(), TEXT("GraphBreadcrumbButtonText")))
		.GraphToEdit(InputSequenceAsset->EdGraph)
		];
}

void FInputSequenceAssetEditor::CreateCommandList()
{
	if (GraphEditorCommands.IsValid()) return;

	GraphEditorCommands = MakeShareable(new FUICommandList);

	GraphEditorCommands->MapAction(FGenericCommands::Get().SelectAll,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::SelectAllNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanSelectAllNodes)
	);

	GraphEditorCommands->MapAction(FGenericCommands::Get().Delete,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::DeleteSelectedNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanDeleteNodes)
	);

	GraphEditorCommands->MapAction(FGenericCommands::Get().Copy,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CopySelectedNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanCopyNodes)
	);

	GraphEditorCommands->MapAction(FGenericCommands::Get().Cut,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CutSelectedNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanCutNodes)
	);

	GraphEditorCommands->MapAction(FGenericCommands::Get().Paste,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::PasteNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanPasteNodes)
	);

	GraphEditorCommands->MapAction(FGenericCommands::Get().Duplicate,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::DuplicateNodes),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanDuplicateNodes)
	);

	GraphEditorCommands->MapAction(
		FGraphEditorCommands::Get().CreateComment,
		FExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::OnCreateComment),
		FCanExecuteAction::CreateRaw(this, &FInputSequenceAssetEditor::CanCreateComment)
	);
}

void FInputSequenceAssetEditor::OnSelectionChanged(const TSet<UObject*>& selectedNodes)
{
	if (selectedNodes.Num() == 1)
	{
		if (UInputSequenceGraphNode_Input* inputNode = Cast<UInputSequenceGraphNode_Input>(*selectedNodes.begin()))
		{
			return DetailsView->SetObject(inputNode);
		}

		if (UEdGraphNode_Comment* commentNode = Cast<UEdGraphNode_Comment>(*selectedNodes.begin()))
		{
			return DetailsView->SetObject(commentNode);
		}
	}
	
	return DetailsView->SetObject(InputSequenceAsset);;
}

void FInputSequenceAssetEditor::OnNodeTitleCommitted(const FText& NewText, ETextCommit::Type CommitInfo, UEdGraphNode* NodeBeingChanged)
{
	if (NodeBeingChanged)
	{
		const FScopedTransaction Transaction(LOCTEXT("K2_RenameNode", "Rename Node"));
		NodeBeingChanged->Modify();
		NodeBeingChanged->OnRenameNode(NewText.ToString());
	}
}

void FInputSequenceAssetEditor::PostUndo(bool bSuccess)
{
	if (bSuccess)
	{
		// Clear selection, to avoid holding refs to nodes that go away
		if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
		{
			if (graphEditor.IsValid())
			{
				graphEditor->ClearSelectionSet();
				graphEditor->NotifyGraphChanged();
			}
		}
		FSlateApplication::Get().DismissAllMenus();
	}
}

void FInputSequenceAssetEditor::PostRedo(bool bSuccess)
{
	if (bSuccess)
	{
		// Clear selection, to avoid holding refs to nodes that go away
		if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
		{
			if (graphEditor.IsValid())
			{
				graphEditor->ClearSelectionSet();
				graphEditor->NotifyGraphChanged();
			}
		}
		FSlateApplication::Get().DismissAllMenus();
	}
}

FGraphPanelSelectionSet FInputSequenceAssetEditor::GetSelectedNodes() const
{
	FGraphPanelSelectionSet CurrentSelection;

	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			CurrentSelection = graphEditor->GetSelectedNodes();
		}
	}

	return CurrentSelection;
}

void FInputSequenceAssetEditor::SelectAllNodes()
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			graphEditor->SelectAllNodes();
		}
	}
}

void FInputSequenceAssetEditor::DeleteSelectedNodes()
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			const FScopedTransaction Transaction(FGenericCommands::Get().Delete->GetDescription());

			graphEditor->GetCurrentGraph()->Modify();

			const FGraphPanelSelectionSet SelectedNodes = graphEditor->GetSelectedNodes();
			graphEditor->ClearSelectionSet();

			for (FGraphPanelSelectionSet::TConstIterator NodeIt(SelectedNodes); NodeIt; ++NodeIt)
			{
				if (UEdGraphNode* Node = Cast<UEdGraphNode>(*NodeIt))
				{
					if (Node->CanUserDeleteNode())
					{
						Node->Modify();
						Node->DestroyNode();
					}
				}
			}
		}
	}
}

bool FInputSequenceAssetEditor::CanDeleteNodes() const
{
	const FGraphPanelSelectionSet SelectedNodes = GetSelectedNodes();
	for (FGraphPanelSelectionSet::TConstIterator SelectedIter(SelectedNodes); SelectedIter; ++SelectedIter)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter);
		if (Node && Node->CanUserDeleteNode()) return true;
	}

	return false;
}

void FInputSequenceAssetEditor::CopySelectedNodes()
{
	TSet<UEdGraphNode*> pressGraphNodes;
	TSet<UEdGraphNode*> releaseGraphNodes;

	FGraphPanelSelectionSet InitialSelectedNodes = GetSelectedNodes();

	for (FGraphPanelSelectionSet::TIterator SelectedIter(InitialSelectedNodes); SelectedIter; ++SelectedIter)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter);

		if (Cast<UInputSequenceGraphNode_Press>(Node)) pressGraphNodes.FindOrAdd(Node);
		if (Cast<UInputSequenceGraphNode_Release>(Node)) releaseGraphNodes.FindOrAdd(Node);
	}

	TSet<UEdGraphNode*> graphNodesToSelect;

	for (UEdGraphNode* pressGraphNode : pressGraphNodes)
	{
		for (UEdGraphPin* pin : pressGraphNode->Pins)
		{
			if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action &&
				pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* linkedGraphNode = pin->LinkedTo[0]->GetOwningNode();

				if (!releaseGraphNodes.Contains(linkedGraphNode) && !graphNodesToSelect.Contains(linkedGraphNode))
				{
					graphNodesToSelect.Add(linkedGraphNode);
				}
			}
		}
	}

	for (UEdGraphNode* releaseGraphNode : releaseGraphNodes)
	{
		for (UEdGraphPin* pin : releaseGraphNode->Pins)
		{
			if (pin->PinType.PinCategory == UInputSequenceGraphSchema::PC_Action &&
				pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* linkedGraphNode = pin->LinkedTo[0]->GetOwningNode();

				if (!pressGraphNodes.Contains(linkedGraphNode) && !graphNodesToSelect.Contains(linkedGraphNode))
				{
					graphNodesToSelect.Add(linkedGraphNode);
				}
			}
		}
	}

	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{

			for (UEdGraphNode* graphNodeToSelect : graphNodesToSelect)
			{
				graphEditor->SetNodeSelection(graphNodeToSelect, true);
			}
		}
	}

	FGraphPanelSelectionSet SelectedNodes = GetSelectedNodes();

	for (FGraphPanelSelectionSet::TIterator SelectedIter(SelectedNodes); SelectedIter; ++SelectedIter)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter);

		if (Node == nullptr)
		{
			SelectedIter.RemoveCurrent();
			continue;
		}

		Node->PrepareForCopying();
	}

	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(SelectedNodes, ExportedText);
	FPlatformApplicationMisc::ClipboardCopy(*ExportedText);
}

bool FInputSequenceAssetEditor::CanCopyNodes() const
{
	const FGraphPanelSelectionSet SelectedNodes = GetSelectedNodes();
	for (FGraphPanelSelectionSet::TConstIterator SelectedIter(SelectedNodes); SelectedIter; ++SelectedIter)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter);
		if (Node && Node->CanDuplicateNode()) return true;
	}

	return false;
}

void FInputSequenceAssetEditor::DeleteSelectedDuplicatableNodes()
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			const FGraphPanelSelectionSet OldSelectedNodes = graphEditor->GetSelectedNodes();
			graphEditor->ClearSelectionSet();

			for (FGraphPanelSelectionSet::TConstIterator SelectedIter(OldSelectedNodes); SelectedIter; ++SelectedIter)
			{
				UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter);
				if (Node && Node->CanDuplicateNode())
				{
					graphEditor->SetNodeSelection(Node, true);
				}
			}

			DeleteSelectedNodes();

			graphEditor->ClearSelectionSet();

			for (FGraphPanelSelectionSet::TConstIterator SelectedIter(OldSelectedNodes); SelectedIter; ++SelectedIter)
			{
				if (UEdGraphNode* Node = Cast<UEdGraphNode>(*SelectedIter))
				{
					graphEditor->SetNodeSelection(Node, true);
				}
			}
		}
	}
}

void FInputSequenceAssetEditor::PasteNodes()
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			FVector2D Location = graphEditor->GetPasteLocation();

			UEdGraph* EdGraph = graphEditor->GetCurrentGraph();

			// Undo/Redo support
			const FScopedTransaction Transaction(FGenericCommands::Get().Paste->GetDescription());

			EdGraph->Modify();

			// Clear the selection set (newly pasted stuff will be selected)
			graphEditor->ClearSelectionSet();

			// Grab the text to paste from the clipboard.
			FString TextToImport;
			FPlatformApplicationMisc::ClipboardPaste(TextToImport);

			// Import the nodes
			TSet<UEdGraphNode*> PastedNodes;
			FEdGraphUtilities::ImportNodesFromText(EdGraph, TextToImport, /*out*/ PastedNodes);

			//Average position of nodes so we can move them while still maintaining relative distances to each other
			FVector2D AvgNodePosition(0.0f, 0.0f);

			for (TSet<UEdGraphNode*>::TIterator It(PastedNodes); It; ++It)
			{
				UEdGraphNode* Node = *It;
				AvgNodePosition.X += Node->NodePosX;
				AvgNodePosition.Y += Node->NodePosY;
			}

			if (PastedNodes.Num() > 0)
			{
				float InvNumNodes = 1.0f / float(PastedNodes.Num());
				AvgNodePosition.X *= InvNumNodes;
				AvgNodePosition.Y *= InvNumNodes;
			}

			for (TSet<UEdGraphNode*>::TIterator It(PastedNodes); It; ++It)
			{
				UEdGraphNode* Node = *It;

				// Select the newly pasted stuff
				graphEditor->SetNodeSelection(Node, true);

				Node->NodePosX = (Node->NodePosX - AvgNodePosition.X) + Location.X;
				Node->NodePosY = (Node->NodePosY - AvgNodePosition.Y) + Location.Y;

				Node->SnapToGrid(GetDefault<UEditorStyleSettings>()->GridSnapSize);

				// Give new node a different Guid from the old one
				Node->CreateNewGuid();
			}

			EdGraph->NotifyGraphChanged();

			InputSequenceAsset->PostEditChange();
			InputSequenceAsset->MarkPackageDirty();
		}
	}
}

bool FInputSequenceAssetEditor::CanPasteNodes() const
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			FString ClipboardContent;
			FPlatformApplicationMisc::ClipboardPaste(ClipboardContent);

			return FEdGraphUtilities::CanImportNodesFromText(graphEditor->GetCurrentGraph(), ClipboardContent);
		}
	}

	return false;
}

void FInputSequenceAssetEditor::OnCreateComment()
{
	if (TSharedPtr<SGraphEditor> graphEditor = GraphEditorPtr.Pin())
	{
		if (graphEditor.IsValid())
		{
			TSharedPtr<FEdGraphSchemaAction> Action = graphEditor->GetCurrentGraph()->GetSchema()->GetCreateCommentAction();
			TSharedPtr<FInputSequenceGraphSchemaAction_NewComment> newCommentAction = StaticCastSharedPtr<FInputSequenceGraphSchemaAction_NewComment>(Action);

			if (newCommentAction.IsValid())
			{
				graphEditor->GetBoundsForSelectedNodes(newCommentAction->SelectedNodesBounds, 50);
				newCommentAction->PerformAction(graphEditor->GetCurrentGraph(), nullptr, FVector2D());
			}
		}
	}
}

bool FInputSequenceAssetEditor::CanCreateComment() const
{
	const FGraphPanelSelectionSet SelectedNodes = GetSelectedNodes();
	return SelectedNodes.Num() > 0;
}

#undef LOCTEXT_NAMESPACE
#pragma endregion