#include "DeathDistribution.h"
#include "Distribute.h"
#include "LinkedDistribution.h"
#include "LookupNPC.h"
#include "PCLevelMultManager.h"
#include "Parser.h"

namespace DeathDistribution
{
#pragma region Parsing

	namespace INI
	{
		struct DeathKeyComponentParser
		{
			template <Distribution::INI::concepts::typed_data Data>
			bool operator()(const std::string& key, Data& data) const
			{
				if (!key.starts_with("Death"sv)) {
					return false;
				}

				std::string rawType = key.substr(5);
				auto        type = RECORD::GetType(rawType);

				if (type == RECORD::kTotal) {
					throw Distribution::INI::Exception::UnsupportedFormTypeException(rawType);
				}
				data.type = type;

				return true;
			}
		};

		using DeathData = Distribution::INI::Data;
		using DataVec = Distribution::INI::DataVec;

		Map<RECORD::TYPE, DataVec> deathConfigs{};

		bool TryParse(const std::string& key, const std::string& value, const Path& path)
		{
			using namespace Distribution::INI;

			try {
				if (auto optData = Parse<DeathData,
						DeathKeyComponentParser,
						DistributableFormComponentParser,
						StringFiltersComponentParser<>,
						FormFiltersComponentParser<>,
						LevelFiltersComponentParser,
						TraitsFilterComponentParser,
						IndexOrCountComponentParser,
						ChanceComponentParser>(key, value);
					optData) {
					auto& data = *optData;
					data.path = path;

					deathConfigs[data.type].emplace_back(data);
				} else {
					return false;
				}
			} catch (const std::exception& e) {
				logger::warn("\t\tFailed to parse entry [{} = {}]: {}", key, value, e.what());
			}
			return true;
		}
	}

#pragma endregion

#pragma region Lookup

	void Manager::LookupForms(RE::TESDataHandler* const dataHandler)
	{
		using namespace Forms;

		ForEachDistributable([&]<typename Form>(Distributables<Form>& a_distributable) {
			// If it's spells distributable we want to manually lookup forms to pick LevSpells that are added into the list.
			if constexpr (!std::is_same_v<Form, RE::SpellItem>) {
				const auto& recordName = RECORD::GetTypeName(a_distributable.GetType());

				a_distributable.LookupForms(dataHandler, recordName, INI::deathConfigs[a_distributable.GetType()]);
			}
		});

		// Sort out Spells and Leveled Spells into two separate lists.
		auto& rawSpells = INI::deathConfigs[RECORD::kSpell];

		for (auto& rawSpell : rawSpells) {
			LookupGenericForm<RE::TESForm>(dataHandler, rawSpell, [&](bool isValid, auto form, const auto& idxOrCount, const auto& filters, const auto& path) {
				if (const auto spell = form->As<RE::SpellItem>(); spell) {
					spells.EmplaceForm(isValid, spell, idxOrCount, filters, path);
				} else if (const auto levSpell = form->As<RE::TESLevSpell>(); levSpell) {
					levSpells.EmplaceForm(isValid, levSpell, idxOrCount, filters, path);
				}
			});
		}

		auto& genericForms = INI::deathConfigs[RECORD::kForm];

		for (auto& rawForm : genericForms) {
			// Add to appropriate list. (Note that type inferring doesn't recognize SleepOutfit, Skin)
			LookupGenericForm<RE::TESForm>(dataHandler, rawForm, [&](bool isValid, auto form, const auto& idxOrCount, const auto& filters, const auto& path) {
				if (const auto keyword = form->As<RE::BGSKeyword>(); keyword) {
					keywords.EmplaceForm(isValid, keyword, idxOrCount, filters, path);
				} else if (const auto spell = form->As<RE::SpellItem>(); spell) {
					spells.EmplaceForm(isValid, spell, idxOrCount, filters, path);
				} else if (const auto levSpell = form->As<RE::TESLevSpell>(); levSpell) {
					levSpells.EmplaceForm(isValid, levSpell, idxOrCount, filters, path);
				} else if (const auto perk = form->As<RE::BGSPerk>(); perk) {
					perks.EmplaceForm(isValid, perk, idxOrCount, filters, path);
				} else if (const auto item = form->As<RE::TESBoundObject>(); item) {
					items.EmplaceForm(isValid, item, idxOrCount, filters, path);
				} else if (const auto outfit = form->As<RE::BGSOutfit>(); outfit) {
					outfits.EmplaceForm(isValid, outfit, idxOrCount, filters, path);
				} else if (const auto faction = form->As<RE::TESFaction>(); faction) {
					factions.EmplaceForm(isValid, faction, idxOrCount, filters, path);
				} else {
					auto type = form->GetFormType();
					if (type == RE::FormType::kPACK || type == RE::FormType::kFLST) {
						// With generic Form entries we default to RandomCount, so we need to properly convert it to Index if it turned out to be a package.
						Index packageIndex = 1;
						if (std::holds_alternative<RandomCount>(idxOrCount)) {
							auto& count = std::get<RandomCount>(idxOrCount);
							if (!count.IsExact()) {
								logger::warn("\t[{}] Inferred Form is a Package, but specifies a random count instead of index. Min value ({}) of the range will be used as an index.", path, count.min);
							}
							packageIndex = count.min;
						} else {
							packageIndex = std::get<Index>(idxOrCount);
						}
						packages.EmplaceForm(isValid, form, packageIndex, filters, path);
					} else {
						logger::warn("\t[{}] Unsupported Form type: {}", path, type);
					}
				}
			});
		}
	}

	bool Manager::IsEmpty()
	{
		return spells.GetForms().empty() &&
		       perks.GetForms().empty() &&
		       items.GetForms().empty() &&
		       levSpells.GetForms().empty() &&
		       packages.GetForms().empty() &&
		       outfits.GetForms().empty() &&
		       keywords.GetForms().empty() &&
		       factions.GetForms().empty() &&
		       sleepOutfits.GetForms().empty() &&
		       skins.GetForms().empty();
	}

	void Manager::LogFormsLookup()
	{
		if (IsEmpty()) {
			return;
		}

		using namespace Forms;

		logger::info("{:*^50}", "ON DEATH");

		ForEachDistributable([]<typename Form>(Distributables<Form>& a_distributable) {
			const auto& recordName = RECORD::GetTypeName(a_distributable.GetType());

			const auto added = a_distributable.GetSize();
			const auto all = a_distributable.GetLookupCount();

			// Only log entries that are actually present in INIs.
			if (all > 0) {
				logger::info("Registered {}/{} {}s", added, all, recordName);
			}
		});
	}
#pragma endregion

#pragma region Distribution

	void Manager::Register()
	{
		if (INI::deathConfigs.empty()) {
			return;
		}

		RE::TESDeathEvent::GetEventSource()->RegisterSink(GetSingleton());
		logger::info("Registered for {}", typeid(RE::TESDeathEvent).name());
	}

	RE::BSEventNotifyControl Manager::ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*)
	{
		constexpr auto is_NPC = [](auto&& a_ref) {
			return a_ref && !a_ref->IsPlayerRef();
		};

		if (a_event.dead && is_NPC(a_event.actorDying)) {
			const auto actor = a_event.actorDying->As<RE::Actor>();
			const auto npc = actor ? actor->GetNPC() : nullptr;
			if (actor && npc) {
				auto       npcData = NPCData(actor, npc);
				const auto input = PCLevelMult::Input{ actor, npc, false };

				DistributedForms distributedForms{};

				Forms::DistributionSet entries{
					spells.GetForms(),
					perks.GetForms(),
					items.GetForms(),
					levSpells.GetForms(),
					packages.GetForms(),
					outfits.GetForms(),
					keywords.GetForms(),
					factions.GetForms(),
					sleepOutfits.GetForms(),
					skins.GetForms()
				};

				Distribute::Distribute(npcData, input, entries, false, &distributedForms);
				// TODO: We can now log per-NPC distributed forms.

				if (!distributedForms.empty()) {
					LinkedDistribution::Manager::GetSingleton()->ForEachLinkedDistributionSet(LinkedDistribution::kDeath, distributedForms, [&](Forms::DistributionSet& set) {
						Distribute::Distribute(npcData, input, set, true, nullptr);  // TODO: Accumulate forms here? to log what was distributed.
					});
				}
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}
#pragma endregion
}
