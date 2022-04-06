/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/ringtones_box.h"

#include "api/api_ringtones.h"
#include "apiwrap.h"
#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_resolver.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"

namespace {

constexpr auto kDefaultValue = -1;
constexpr auto kNoSoundValue = -2;

} // namespace

QString ExtractRingtoneName(not_null<DocumentData*> document) {
	if (document->isNull()) {
		return QString();
	}
	const auto name = document->filename();
	if (!name.isEmpty()) {
		const auto extension = Data::FileExtension(name);
		if (extension.isEmpty()) {
			return name;
		} else if (name.size() > extension.size() + 1) {
			return name.mid(0, name.size() - extension.size() - 1);
		}
	}
	const auto date = langDateTime(
		base::unixtime::parse(document->date));
	const auto base = document->isVoiceMessage()
		? (tr::lng_in_dlg_audio(tr::now) + ' ')
		: document->isAudioFile()
		? (tr::lng_in_dlg_audio_file(tr::now) + ' ')
		: QString();
	return base + date;
}

void RingtonesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Data::NotifySound selected,
		Fn<void(Data::NotifySound)> save) {
	box->setTitle(tr::lng_ringtones_box_title());

	const auto container = box->verticalLayout();

	auto padding = st::boxPadding;
	padding.setTop(padding.bottom());

	struct State {
		std::shared_ptr<Ui::RadiobuttonGroup> group;
		std::vector<DocumentId> documentIds;
		Data::NotifySound chosen;
		base::unique_qptr<Ui::PopupMenu> menu;
		QPointer<Ui::Radiobutton> defaultButton;
		QPointer<Ui::Radiobutton> chosenButton;
		std::vector<QPointer<Ui::Radiobutton>> buttons;
	};
	const auto state = container->lifetime().make_state<State>(State{
		.group = std::make_shared<Ui::RadiobuttonGroup>(),
		.chosen = selected,
	});

	const auto addToGroup = [=](
			not_null<Ui::VerticalLayout*> verticalLayout,
			int value,
			const QString &text,
			bool chosen) {
		if (chosen) {
			state->group->setValue(value);
		}
		const auto button = verticalLayout->add(
			object_ptr<Ui::Radiobutton>(
				verticalLayout,
				state->group,
				value,
				text,
				st::defaultCheckbox),
			padding);
		if (chosen) {
			state->chosenButton = button;
		}
		if (value == kDefaultValue) {
			state->defaultButton = button;
		}
		if (value < 0) {
			return;
		}
		while (state->buttons.size() <= value) {
			state->buttons.push_back(nullptr);
		}
		base::install_event_filter(button, [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::ContextMenu || state->menu) {
				return base::EventFilterResult::Continue;
			}
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				button,
				st::popupMenuWithIcons);
			auto callback = [=] {
				const auto id = state->documentIds[value];
				session->api().ringtones().remove(id);
			};
			state->menu->addAction(
				tr::lng_box_delete(tr::now),
				std::move(callback),
				&st::menuIconDelete);
			state->menu->popup(QCursor::pos());
			return base::EventFilterResult::Cancel;
		});
	};

	session->api().ringtones().uploadFails(
	) | rpl::start_with_next([=](const QString &error) {
		if ((error == u"RINGTONE_DURATION_TOO_LONG"_q)
			|| (error == u"RINGTONE_SIZE_TOO_BIG"_q)) {
			box->getDelegate()->show(
				Ui::MakeInformBox(tr::lng_ringtones_box_error()));
		} else if (error == u"RINGTONE_MIME_INVALID"_q) {
			box->getDelegate()->show(
				Ui::MakeInformBox(tr::lng_edit_media_invalid_file()));
		}
	}, box->lifetime());

	Settings::AddSubsectionTitle(
		container,
		tr::lng_ringtones_box_cloud_subtitle());

	const auto noSound = selected.none;
	addToGroup(
		container,
		kDefaultValue,
		tr::lng_ringtones_box_default(tr::now),
		false);
	addToGroup(
		container,
		kNoSoundValue,
		tr::lng_ringtones_box_no_sound(tr::now),
		noSound);

	const auto custom = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto rebuild = [=] {
		state->documentIds.clear();
		auto value = 0;
		while (custom->count()) {
			delete custom->widgetAt(0);
		}

		for (const auto &id : session->api().ringtones().list()) {
			const auto chosen = (state->chosen.id && state->chosen.id == id);
			const auto document = session->data().document(id);
			const auto text = ExtractRingtoneName(document);
			addToGroup(custom, value++, text, chosen);
			state->documentIds.push_back(id);
		}

		custom->resizeToWidth(container->width());
		if (!state->chosenButton) {
			state->group->setValue(kDefaultValue);
			state->defaultButton->finishAnimating();
		}
	};

	session->api().ringtones().listUpdates(
	) | rpl::start_with_next(rebuild, container->lifetime());

	session->api().ringtones().uploadDones(
	) | rpl::start_with_next([=](DocumentId id) {
		state->chosen = Data::NotifySound{ .id = id };
		rebuild();
	}, container->lifetime());

	session->api().ringtones().requestList();
	rebuild();

	const auto upload = box->addRow(
		Settings::CreateButton(
			container,
			tr::lng_ringtones_box_upload_button(),
			st::ringtonesBoxButton,
			{
				&st::mainMenuAddAccount,
				0,
				Settings::IconType::Round,
				&st::windowBgActive
			}),
		style::margins());
	upload->addClickHandler([=] {
		const auto delay = st::ringtonesBoxButton.ripple.hideDuration;
		base::call_delayed(delay, crl::guard(box, [=] {
			const auto callback = [=](const FileDialog::OpenResult &result) {
				auto mime = QString();
				auto name = QString();
				auto content = result.remoteContent;
				if (!result.paths.isEmpty()) {
					auto info = QFileInfo(result.paths.front());
					mime = Core::MimeTypeForFile(info).name();
					name = info.fileName();
					auto f = QFile(result.paths.front());
					if (f.open(QIODevice::ReadOnly)) {
						content = f.readAll();
						f.close();
					}
				} else {
					mime = Core::MimeTypeForData(content).name();
					name = "audio";
				}

				session->api().ringtones().upload(name, mime, content);
			};
			FileDialog::GetOpenPath(
				box.get(),
				tr::lng_ringtones_box_upload_choose(tr::now),
				"Audio files (*.mp3)",
				crl::guard(box, callback));
		}));
	});

	box->addSkip(st::ringtonesBoxSkip);
	Settings::AddDividerText(container, tr::lng_ringtones_box_about());

	box->addSkip(st::ringtonesBoxSkip);

	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_settings_save(), [=] {
		const auto value = state->group->value();
		auto sound = (value == kDefaultValue)
			? Data::NotifySound()
			: (value == kNoSoundValue)
			? Data::NotifySound{ .none = true }
		: Data::NotifySound{ .id = state->documentIds[value] };
		save(sound);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void PeerRingtonesBox(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> peer) {
	const auto now = peer->owner().notifySettings().sound(peer);
	RingtonesBox(box, &peer->session(), now, [=](Data::NotifySound sound) {
		peer->owner().notifySettings().update(peer, {}, {}, sound);
	});
}