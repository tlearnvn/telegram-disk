// ---------------------------------------------------------------------------
//  Schema API Telegram — phần được Tuấn's Telegram Disk sử dụng.
//
//  ĐÂY LÀ TẬP CON của tệp api.tl chính thức, chỉ gồm những hàm dựng mà ứng dụng
//  thực sự cần. Muốn dùng bản đầy đủ / layer mới hơn, chỉ cần đặt tệp api.tl
//  chính thức vào thư mục `schema/` cạnh tệp thực thi (hoặc trỏ tới nó bằng
//  cấu hình `telegram.schema_file`) — ứng dụng sẽ tự nạp và ưu tiên tệp đó.
//
//  Những dòng có ghi chú "// đã đối chiếu" nghĩa là #id trùng khớp với CRC32
//  của chuỗi khai báo — hai giá trị xác nhận lẫn nhau. Các dòng KHÔNG ghi #id
//  sẽ được tính ID tự động từ chính chuỗi khai báo.
//
//  Bộ giải mã hỗ trợ "giải mã một phần": nếu gặp hàm dựng lạ ở trường lồng
//  bên trong, các trường đã đọc được vẫn giữ nguyên, nên đường dẫn tải/ghi
//  tệp vẫn hoạt động ngay cả khi schema hơi lệch so với máy chủ.
//
// LAYER 158
// ---------------------------------------------------------------------------

---types---

// --- Kiểu cơ bản -----------------------------------------------------------
inputPeerEmpty#7f3b18ea = InputPeer;
inputPeerSelf#7da07ec9 = InputPeer;
inputPeerChat#35a95cb9 chat_id:long = InputPeer;
inputPeerUser#dde8a54c user_id:long access_hash:long = InputPeer;
inputPeerChannel#27bcbbfc channel_id:long access_hash:long = InputPeer;
inputPeerUserFromMessage#a87b0a1c peer:InputPeer msg_id:int user_id:long = InputPeer;
inputPeerChannelFromMessage#bd2a0840 peer:InputPeer msg_id:int channel_id:long = InputPeer;

inputUserEmpty#b98886cf = InputUser;
inputUserSelf#f7c1b13f = InputUser;
inputUser#f21158c6 user_id:long access_hash:long = InputUser;

inputChannelEmpty#ee8c1e86 = InputChannel;
inputChannel#f35aec28 channel_id:long access_hash:long = InputChannel;
inputChannelFromMessage#5b934f9d peer:InputPeer msg_id:int channel_id:long = InputChannel;

peerUser#59511722 user_id:long = Peer;
peerChat#36c6019a chat_id:long = Peer;
peerChannel#a2a5371e channel_id:long = Peer;

// --- Tệp -------------------------------------------------------------------
inputFile#f52ff27f id:long parts:int name:string md5_checksum:string = InputFile;
inputFileBig#fa4f0bb5 id:long parts:int name:string = InputFile;

inputFileLocation#dfdaabe1 volume_id:long local_id:int secret:long file_reference:bytes = InputFileLocation;
inputDocumentFileLocation#bad07584 id:long access_hash:long file_reference:bytes thumb_size:string = InputFileLocation;

inputDocumentEmpty#72f0eaae = InputDocument;
inputDocument#1abfb575 id:long access_hash:long file_reference:bytes = InputDocument;

storage.fileUnknown#aa963b05 = storage.FileType;
storage.filePartial#40bc6f52 = storage.FileType;
storage.fileJpeg#7efe0e = storage.FileType;
storage.fileGif#cae1aadf = storage.FileType;
storage.filePng#a4f63c0 = storage.FileType;
storage.filePdf#ae1e508d = storage.FileType;
storage.fileMp3#528a0677 = storage.FileType;
storage.fileMov#4b09ebbc = storage.FileType;
storage.fileMp4#b3cea0e4 = storage.FileType;
storage.fileWebp#1081464c = storage.FileType;

upload.file#096a18d5 type:storage.FileType mtime:int bytes:bytes = upload.File;
upload.fileCdnRedirect#f18cda44 dc_id:int file_token:bytes encryption_key:bytes encryption_iv:bytes file_hashes:Vector<FileHash> = upload.File;
fileHash#f39b035c offset:long limit:int hash:bytes = FileHash;

// --- Tài liệu --------------------------------------------------------------
documentEmpty#36f8c871 id:long = Document;
document#8fd4c4d8 flags:# id:long access_hash:long file_reference:bytes date:int mime_type:string size:long thumbs:flags.0?Vector<PhotoSize> video_thumbs:flags.1?Vector<VideoSize> dc_id:int attributes:Vector<DocumentAttribute> = Document;

documentAttributeImageSize#6c37c15c w:int h:int = DocumentAttribute;
documentAttributeAnimated#11b58939 = DocumentAttribute;
documentAttributeSticker flags:# mask:flags.1?true alt:string stickerset:InputStickerSet mask_coords:flags.0?MaskCoords = DocumentAttribute;
documentAttributeVideo flags:# round_message:flags.0?true supports_streaming:flags.1?true duration:int w:int h:int preload_prefix_size:flags.2?int = DocumentAttribute;
documentAttributeAudio flags:# voice:flags.10?true duration:int title:flags.0?string performer:flags.1?string waveform:flags.2?bytes = DocumentAttribute;
documentAttributeFilename#15590068 file_name:string = DocumentAttribute;
documentAttributeHasStickers#9801d2f7 = DocumentAttribute;
documentAttributeCustomEmoji flags:# free:flags.0?true text_color:flags.1?true alt:string stickerset:InputStickerSet = DocumentAttribute;

photoSizeEmpty#0e17e23c type:string = PhotoSize;
photoSize#75c78e60 type:string w:int h:int size:int = PhotoSize;
photoCachedSize#021e1ad6 type:string w:int h:int bytes:bytes = PhotoSize;
photoStrippedSize#e0b0bc2e type:string bytes:bytes = PhotoSize;
photoSizeProgressive#fa3efb95 type:string w:int h:int sizes:Vector<int> = PhotoSize;
photoPathSize#d8214d41 type:string bytes:bytes = PhotoSize;
videoSize flags:# type:string w:int h:int size:int video_start_ts:flags.0?double = VideoSize;
videoSizeEmojiMarkup#f85c413c emoji_id:long background_colors:Vector<int> = VideoSize;
videoSizeStickerMarkup#da082fe stickerset:InputStickerSet sticker_id:long background_colors:Vector<int> = VideoSize;

inputStickerSetEmpty#ffb62b95 = InputStickerSet;
inputStickerSetID#9de7a269 id:long access_hash:long = InputStickerSet;
inputStickerSetShortName#861cc8a0 short_name:string = InputStickerSet;
maskCoords#aed6dbb2 n:int x:double y:double zoom:double = MaskCoords;

// --- Phương tiện trong tin nhắn --------------------------------------------
inputMediaEmpty#9664f57f = InputMedia;
inputMediaUploadedDocument#5b38c6c1 flags:# nosound_video:flags.3?true force_file:flags.4?true spoiler:flags.5?true file:InputFile thumb:flags.2?InputFile mime_type:string attributes:Vector<DocumentAttribute> stickers:flags.0?Vector<InputDocument> ttl_seconds:flags.1?int = InputMedia;
inputMediaDocument flags:# spoiler:flags.2?true id:InputDocument ttl_seconds:flags.0?int query:flags.1?string = InputMedia;

messageMediaEmpty#3ded6320 = MessageMedia;
messageMediaDocument#4cf4d72d flags:# nopremium:flags.3?true spoiler:flags.4?true document:flags.0?Document alt_document:flags.5?Document ttl_seconds:flags.2?int = MessageMedia;
messageMediaUnsupported#9f84f49e = MessageMedia;

// --- Tin nhắn --------------------------------------------------------------
messageEmpty#90a6ca84 flags:# id:int peer_id:flags.0?Peer = Message;
message#38116ee0 flags:# out:flags.1?true mentioned:flags.4?true media_unread:flags.5?true silent:flags.13?true post:flags.14?true from_scheduled:flags.18?true legacy:flags.19?true edit_hide:flags.21?true pinned:flags.24?true noforwards:flags.26?true id:int from_id:flags.8?Peer peer_id:Peer fwd_from:flags.2?MessageFwdHeader via_bot_id:flags.11?long reply_to:flags.3?MessageReplyHeader date:int message:string media:flags.9?MessageMedia reply_markup:flags.6?ReplyMarkup entities:flags.7?Vector<MessageEntity> views:flags.10?int forwards:flags.10?int replies:flags.23?MessageReplies edit_date:flags.15?int post_author:flags.16?string grouped_id:flags.17?long reactions:flags.20?MessageReactions restriction_reason:flags.22?Vector<RestrictionReason> ttl_period:flags.25?int = Message;
messageService flags:# out:flags.1?true mentioned:flags.4?true media_unread:flags.5?true silent:flags.13?true post:flags.14?true legacy:flags.19?true id:int from_id:flags.8?Peer peer_id:Peer reply_to:flags.3?MessageReplyHeader date:int action:MessageAction ttl_period:flags.25?int = Message;

messageReplyHeader flags:# reply_to_scheduled:flags.2?true forum_topic:flags.3?true reply_to_msg_id:int reply_to_peer_id:flags.0?Peer reply_to_top_id:flags.1?int = MessageReplyHeader;
messageFwdHeader flags:# imported:flags.7?true from_id:flags.0?Peer from_name:flags.5?string date:int channel_post:flags.2?int post_author:flags.3?string saved_from_peer:flags.4?Peer saved_from_msg_id:flags.4?int psa_type:flags.6?string = MessageFwdHeader;
messageReplies flags:# comments:flags.0?true replies:int replies_pts:int recent_repliers:flags.1?Vector<Peer> channel_id:flags.0?long max_id:flags.2?int read_max_id:flags.3?int = MessageReplies;
messageReactions flags:# min:flags.0?true can_see_list:flags.2?true results:Vector<ReactionCount> recent_reactions:flags.1?Vector<MessagePeerReaction> = MessageReactions;
reactionCount flags:# chosen_order:flags.0?int reaction:Reaction count:int = ReactionCount;
messagePeerReaction flags:# big:flags.0?true unread:flags.1?true my:flags.2?true peer_id:Peer date:int reaction:Reaction = MessagePeerReaction;
reactionEmpty#79f5d419 = Reaction;
reactionEmoji#1b2286b8 emoticon:string = Reaction;
reactionCustomEmoji#8935fc73 document_id:long = Reaction;

messageEntityUnknown#bb92ba95 offset:int length:int = MessageEntity;
messageEntityMention#fa04579d offset:int length:int = MessageEntity;
messageEntityHashtag#6f635b0d offset:int length:int = MessageEntity;
messageEntityBotCommand#6cef8ac7 offset:int length:int = MessageEntity;
messageEntityUrl#6ed02538 offset:int length:int = MessageEntity;
messageEntityEmail#64e475c2 offset:int length:int = MessageEntity;
messageEntityBold#bd610bc9 offset:int length:int = MessageEntity;
messageEntityItalic#826f8b60 offset:int length:int = MessageEntity;
messageEntityCode#28a20571 offset:int length:int = MessageEntity;
messageEntityPre#73924be0 offset:int length:int language:string = MessageEntity;
messageEntityTextUrl#76a6d327 offset:int length:int url:string = MessageEntity;
messageEntityMentionName#dc7b1140 offset:int length:int user_id:long = MessageEntity;
messageEntityPhone#9b69e34b offset:int length:int = MessageEntity;
messageEntityCashtag#4c4e743f offset:int length:int = MessageEntity;
messageEntityUnderline#9c4e7e8b offset:int length:int = MessageEntity;
messageEntityStrike#bf0693d4 offset:int length:int = MessageEntity;
messageEntityBlockquote#020df5d0 offset:int length:int = MessageEntity;
messageEntityBankCard#761e6af4 offset:int length:int = MessageEntity;
messageEntitySpoiler#32ca960f offset:int length:int = MessageEntity;
messageEntityCustomEmoji#c8cf05f8 offset:int length:int document_id:long = MessageEntity;

// --- Chat / kênh -----------------------------------------------------------
chatEmpty#29562865 id:long = Chat;
chat flags:# creator:flags.0?true left:flags.2?true deactivated:flags.5?true call_active:flags.23?true call_not_empty:flags.24?true noforwards:flags.25?true id:long title:string photo:ChatPhoto participants_count:int date:int version:int migrated_to:flags.6?InputChannel admin_rights:flags.14?ChatAdminRights default_banned_rights:flags.18?ChatBannedRights = Chat;
chatForbidden#6592a1a7 id:long title:string = Chat;
channel#83259464 flags:# creator:flags.0?true left:flags.2?true broadcast:flags.5?true verified:flags.7?true megagroup:flags.8?true restricted:flags.9?true signatures:flags.11?true min:flags.12?true scam:flags.19?true has_link:flags.20?true has_geo:flags.21?true slowmode_enabled:flags.22?true call_active:flags.23?true call_not_empty:flags.24?true fake:flags.25?true gigagroup:flags.26?true noforwards:flags.27?true join_to_send:flags.28?true join_request:flags.29?true forum:flags.30?true flags2:# id:long access_hash:flags.13?long title:string username:flags.6?string photo:ChatPhoto date:int restriction_reason:flags.9?Vector<RestrictionReason> admin_rights:flags.14?ChatAdminRights banned_rights:flags.15?ChatBannedRights default_banned_rights:flags.18?ChatBannedRights participants_count:flags.17?int usernames:flags2.0?Vector<Username> = Chat;
channelForbidden#17d493d5 flags:# broadcast:flags.5?true megagroup:flags.8?true id:long access_hash:long title:string until_date:flags.16?int = Chat;

chatPhotoEmpty#37c1011c = ChatPhoto;
chatPhoto flags:# has_video:flags.0?true photo_id:long stripped_thumb:flags.1?bytes dc_id:int = ChatPhoto;

chatAdminRights#5fb224d5 flags:# change_info:flags.0?true post_messages:flags.1?true edit_messages:flags.2?true delete_messages:flags.3?true ban_users:flags.4?true invite_users:flags.5?true pin_messages:flags.7?true add_admins:flags.9?true anonymous:flags.10?true manage_call:flags.11?true other:flags.12?true manage_topics:flags.13?true = ChatAdminRights;
chatBannedRights#9f120418 flags:# view_messages:flags.0?true send_messages:flags.1?true send_media:flags.2?true send_stickers:flags.3?true send_gifs:flags.4?true send_games:flags.5?true send_inline:flags.6?true embed_links:flags.7?true send_polls:flags.8?true change_info:flags.10?true invite_users:flags.15?true pin_messages:flags.17?true manage_topics:flags.18?true until_date:int = ChatBannedRights;

restrictionReason#d072acb4 platform:string reason:string text:string = RestrictionReason;
username flags:# editable:flags.0?true active:flags.1?true username:string = Username;

// --- Người dùng ------------------------------------------------------------
userEmpty#d3bc4b7a id:long = User;
user#8f97c628 flags:# self:flags.10?true contact:flags.11?true mutual_contact:flags.12?true deleted:flags.13?true bot:flags.14?true bot_chat_history:flags.15?true bot_nochats:flags.16?true verified:flags.17?true restricted:flags.18?true min:flags.20?true bot_inline_geo:flags.21?true support:flags.23?true scam:flags.24?true apply_min_photo:flags.25?true fake:flags.26?true bot_attach_menu:flags.27?true premium:flags.28?true attach_menu_enabled:flags.29?true flags2:# id:long access_hash:flags.0?long first_name:flags.1?string last_name:flags.2?string username:flags.3?string phone:flags.4?string photo:flags.5?UserProfilePhoto status:flags.6?UserStatus bot_info_version:flags.14?int restriction_reason:flags.18?Vector<RestrictionReason> bot_inline_placeholder:flags.19?string lang_code:flags.22?string emoji_status:flags.30?EmojiStatus usernames:flags2.0?Vector<Username> = User;

userProfilePhotoEmpty#4f11bae1 = UserProfilePhoto;
userProfilePhoto flags:# has_video:flags.0?true personal:flags.2?true photo_id:long stripped_thumb:flags.1?bytes dc_id:int = UserProfilePhoto;

userStatusEmpty#09d05049 = UserStatus;
userStatusOnline#edb93949 expires:int = UserStatus;
userStatusOffline#008c703f was_online:int = UserStatus;
userStatusRecently#e26f42f1 = UserStatus;
userStatusLastWeek#07bf09fc = UserStatus;
userStatusLastMonth#77ebc742 = UserStatus;

emojiStatusEmpty#2de11aae = EmojiStatus;
emojiStatus#929b619d document_id:long = EmojiStatus;
emojiStatusUntil#fa30a8c7 document_id:long until:int = EmojiStatus;

// --- Cập nhật --------------------------------------------------------------
updatesTooLong#e317af7e = Updates;
updateShortMessage#313bc7f8 flags:# out:flags.1?true mentioned:flags.4?true media_unread:flags.5?true silent:flags.13?true id:int user_id:long message:string pts:int pts_count:int date:int fwd_from:flags.2?MessageFwdHeader via_bot_id:flags.11?long reply_to:flags.3?MessageReplyHeader entities:flags.7?Vector<MessageEntity> ttl_period:flags.25?int = Updates;
updateShortChatMessage#4d6deea5 flags:# out:flags.1?true mentioned:flags.4?true media_unread:flags.5?true silent:flags.13?true id:int from_id:long chat_id:long message:string pts:int pts_count:int date:int fwd_from:flags.2?MessageFwdHeader via_bot_id:flags.11?long reply_to:flags.3?MessageReplyHeader entities:flags.7?Vector<MessageEntity> ttl_period:flags.25?int = Updates;
updateShort#78d4dec1 update:Update date:int = Updates;
updatesCombined#725b04c3 updates:Vector<Update> users:Vector<User> chats:Vector<Chat> date:int seq_start:int seq:int = Updates;
updates#74ae4240 updates:Vector<Update> users:Vector<User> chats:Vector<Chat> date:int seq:int = Updates;
updateShortSentMessage#9015e101 flags:# out:flags.1?true id:int pts:int pts_count:int date:int media:flags.9?MessageMedia entities:flags.7?Vector<MessageEntity> ttl_period:flags.25?int = Updates;

updateNewMessage#1f2b0afd message:Message pts:int pts_count:int = Update;
updateMessageID#4e90bfd6 id:int random_id:long = Update;
updateNewChannelMessage#62ba04d9 message:Message pts:int pts_count:int = Update;
updateDeleteMessages#a20db0e5 messages:Vector<int> pts:int pts_count:int = Update;
updateDeleteChannelMessages#c32d5b12 channel_id:long messages:Vector<int> pts:int pts_count:int = Update;
updateEditChannelMessage#1b3f4df7 message:Message pts:int pts_count:int = Update;
updateEditMessage#e40370a3 message:Message pts:int pts_count:int = Update;

// --- Cấu hình --------------------------------------------------------------
config#cc1a241e flags:# phonecalls_enabled:flags.1?true default_p2p_contacts:flags.3?true preload_featured_stickers:flags.4?true revoke_pm_inbox:flags.6?true blocked_mode:flags.8?true force_try_ipv6:flags.14?true date:int expires:int test_mode:Bool this_dc:int dc_options:Vector<DcOption> dc_txt_domain_name:string chat_size_max:int megagroup_size_max:int forwarded_count_max:int online_update_period_ms:int offline_blur_timeout_ms:int offline_idle_timeout_ms:int online_cloud_timeout_ms:int notify_cloud_delay_ms:int notify_default_delay_ms:int push_chat_period_ms:int push_chat_limit:int edit_time_limit:int revoke_time_limit:int revoke_pm_time_limit:int rating_e_decay:int stickers_recent_limit:int channels_read_media_period:int tmp_sessions:flags.0?int call_receive_timeout_ms:int call_ring_timeout_ms:int call_connect_timeout_ms:int call_packet_timeout_ms:int me_url_prefix:string autoupdate_url_prefix:flags.7?string gif_search_username:flags.9?string venue_search_username:flags.10?string img_search_username:flags.11?string static_maps_provider:flags.12?string caption_length_max:int message_length_max:int webfile_dc_id:int suggested_lang_code:flags.2?string lang_pack_version:flags.2?int base_lang_pack_version:flags.2?int reactions_default:flags.15?Reaction autologin_token:flags.16?string = Config;
dcOption#18b7a10d flags:# ipv6:flags.0?true media_only:flags.1?true tcpo_only:flags.2?true cdn:flags.3?true static:flags.4?true this_port_only:flags.5?true id:int ip_address:string port:int secret:flags.10?bytes = DcOption;
nearestDc#8e1a1775 country:string this_dc:int nearest_dc:int = NearestDc;

// --- Xác thực --------------------------------------------------------------
auth.sentCode#5e002502 flags:# type:auth.SentCodeType phone_code_hash:string next_type:flags.1?auth.CodeType timeout:flags.2?int = auth.SentCode;
auth.sentCodeTypeApp#3dbb5986 length:int = auth.SentCodeType;
auth.sentCodeTypeSms#c000bba2 length:int = auth.SentCodeType;
auth.sentCodeTypeCall#5353e5a7 length:int = auth.SentCodeType;
auth.sentCodeTypeFlashCall#ab03c6d9 pattern:string = auth.SentCodeType;
auth.sentCodeTypeMissedCall#82006484 prefix:string length:int = auth.SentCodeType;
auth.sentCodeTypeEmailCode flags:# apple_signin_allowed:flags.0?true google_signin_allowed:flags.1?true email_pattern:string length:int reset_available_period:flags.3?int reset_pending_date:flags.4?int = auth.SentCodeType;
auth.sentCodeTypeSetUpEmailRequired flags:# apple_signin_allowed:flags.0?true google_signin_allowed:flags.1?true = auth.SentCodeType;
auth.sentCodeTypeFragmentSms#d9565c39 url:string length:int = auth.SentCodeType;
auth.sentCodeTypeFirebaseSms flags:# nonce:flags.0?bytes receipt:flags.1?string push_timeout:flags.1?int length:int = auth.SentCodeType;

auth.codeTypeSms#72a3158c = auth.CodeType;
auth.codeTypeCall#741cd3e3 = auth.CodeType;
auth.codeTypeFlashCall#226ccefb = auth.CodeType;
auth.codeTypeMissedCall#d61ad6ee = auth.CodeType;
auth.codeTypeFragmentSms#06ed998c = auth.CodeType;

auth.authorization#2ea2c0d4 flags:# setup_password_required:flags.1?true otherwise_relogin_days:flags.1?int tmp_sessions:flags.0?int future_auth_token:flags.2?bytes user:User = auth.Authorization;
auth.authorizationSignUpRequired#44747e9a flags:# terms_of_service:flags.0?help.TermsOfService = auth.Authorization;
auth.exportedAuthorization#b434e2b8 id:long bytes:bytes = auth.ExportedAuthorization;
auth.loggedOut flags:# future_auth_token:flags.0?bytes = auth.LoggedOut;

// Bản codeSettings tối giản: mọi trường đều là cờ `true` nên phần thân chỉ có
// 4 byte flags — đủ dùng và ổn định qua nhiều layer.
codeSettings#debebe83 flags:# allow_flashcall:flags.0?true current_number:flags.1?true allow_app_hash:flags.4?true = CodeSettings;

account.password#957b50fb flags:# has_recovery:flags.0?true has_secure_values:flags.1?true has_password:flags.2?true current_algo:flags.2?PasswordKdfAlgo srp_B:flags.2?bytes srp_id:flags.2?long hint:flags.3?string email_unconfirmed_pattern:flags.4?string new_algo:PasswordKdfAlgo new_secure_algo:SecurePasswordKdfAlgo secure_random:bytes pending_reset_date:flags.5?int login_email_pattern:flags.6?string = account.Password;
passwordKdfAlgoUnknown#d45ab096 = PasswordKdfAlgo;
passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow#3a912d4a salt1:bytes salt2:bytes g:int p:bytes = PasswordKdfAlgo;
securePasswordKdfAlgoUnknown#004a8537 = SecurePasswordKdfAlgo;
securePasswordKdfAlgoPBKDF2HMACSHA512iter100000#bbf2dda0 salt:bytes = SecurePasswordKdfAlgo;
securePasswordKdfAlgoSHA512#86471d92 salt:bytes = SecurePasswordKdfAlgo;

inputCheckPasswordEmpty#9880f658 = InputCheckPasswordSRP;
inputCheckPasswordSRP#d27ff082 srp_id:long A:bytes M1:bytes = InputCheckPasswordSRP;

// --- Kết quả truy vấn ------------------------------------------------------
messages.chats#64ff9fd5 chats:Vector<Chat> = messages.Chats;
messages.chatsSlice#9cd81144 count:int chats:Vector<Chat> = messages.Chats;

messages.messages#8c718e87 messages:Vector<Message> chats:Vector<Chat> users:Vector<User> = messages.Messages;
messages.messagesSlice flags:# inexact:flags.1?true count:int next_rate:flags.0?int offset_id_offset:flags.2?int messages:Vector<Message> chats:Vector<Chat> users:Vector<User> = messages.Messages;
messages.channelMessages#c776ba4e flags:# inexact:flags.1?true pts:int count:int offset_id_offset:flags.2?int messages:Vector<Message> topics:Vector<ForumTopic> chats:Vector<Chat> users:Vector<User> = messages.Messages;
messages.messagesNotModified#74535f21 count:int = messages.Messages;

messages.affectedMessages#84d19185 pts:int pts_count:int = messages.AffectedMessages;

inputMessageID#a676a322 id:int = InputMessage;
inputMessageReplyTo#bad88395 id:int = InputMessage;
inputMessagePinned#86872538 = InputMessage;

contacts.resolvedPeer#7f077ad9 peer:Peer chats:Vector<Chat> users:Vector<User> = contacts.ResolvedPeer;

messages.dialogs#15ba6c40 dialogs:Vector<Dialog> messages:Vector<Message> chats:Vector<Chat> users:Vector<User> = messages.Dialogs;
messages.dialogsSlice#71e094f3 count:int dialogs:Vector<Dialog> messages:Vector<Message> chats:Vector<Chat> users:Vector<User> = messages.Dialogs;
messages.dialogsNotModified#f0e3e596 count:int = messages.Dialogs;
dialog flags:# pinned:flags.2?true unread_mark:flags.3?true peer:Peer top_message:int read_inbox_max_id:int read_outbox_max_id:int unread_count:int unread_mentions_count:int unread_reactions_count:int notify_settings:PeerNotifySettings pts:flags.0?int draft:flags.1?DraftMessage folder_id:flags.4?int = Dialog;
peerNotifySettings flags:# show_previews:flags.0?Bool silent:flags.1?Bool mute_until:flags.2?int ios_sound:flags.3?NotificationSound android_sound:flags.4?NotificationSound other_sound:flags.5?NotificationSound = PeerNotifySettings;
notificationSoundDefault#97e8bebe = NotificationSound;
notificationSoundNone#6f0c34df = NotificationSound;
notificationSoundLocal#830b9ae4 title:string data:string = NotificationSound;
notificationSoundRingtone#ff6c8049 id:long = NotificationSound;
draftMessageEmpty flags:# date:flags.0?int = DraftMessage;
draftMessage flags:# no_webpage:flags.1?true reply_to_msg_id:flags.0?int message:string entities:flags.3?Vector<MessageEntity> date:int = DraftMessage;

forumTopicDeleted#023f109b id:int = ForumTopic;
forumTopic flags:# my:flags.1?true closed:flags.2?true pinned:flags.3?true short:flags.5?true hidden:flags.6?true id:int date:int title:string icon_color:int icon_emoji_id:flags.0?long top_message:int read_inbox_max_id:int read_outbox_max_id:int unread_count:int unread_mentions_count:int unread_reactions_count:int from_id:Peer notify_settings:PeerNotifySettings draft:flags.4?DraftMessage = ForumTopic;

help.termsOfService#780a0310 flags:# popup:flags.0?true id:DataJSON text:string entities:Vector<MessageEntity> min_age_confirm:flags.1?int = help.TermsOfService;
dataJSON#7d748d04 data:string = DataJSON;

inputClientProxy#75588b3f address:string port:int = InputClientProxy;
jsonNull#3f6d7b68 = JSONValue;
jsonBool#c7345e6a value:Bool = JSONValue;
jsonNumber#2be0dfa4 value:double = JSONValue;
jsonString#b71e767a value:string = JSONValue;
jsonArray#f7444763 value:Vector<JSONValue> = JSONValue;
jsonObject#99c1d49d value:Vector<JSONObjectValue> = JSONValue;
jsonObjectValue#c0de1bd9 key:string value:JSONValue = JSONObjectValue;

---functions---

initConnection#c1cd5ea9 {X:Type} flags:# api_id:int device_model:string system_version:string app_version:string system_lang_code:string lang_pack:string lang_code:string proxy:flags.0?InputClientProxy params:flags.1?JSONValue query:!X = X;

help.getConfig#c4f9186b = Config;
help.getNearestDc#1fb33026 = NearestDc;

auth.sendCode#a677244f phone_number:string api_id:int api_hash:string settings:CodeSettings = auth.SentCode;
auth.signIn#8d52a951 flags:# phone_number:string phone_code_hash:string phone_code:flags.0?string email_verification:flags.1?EmailVerification = auth.Authorization;
auth.logOut#3e72ba19 = auth.LoggedOut;
auth.exportAuthorization#e5bfffcd dc_id:int = auth.ExportedAuthorization;
auth.importAuthorization#a57a7dad id:long bytes:bytes = auth.Authorization;
auth.checkPassword#d18b4d16 password:InputCheckPasswordSRP = auth.Authorization;
auth.resendCode flags:# phone_number:string phone_code_hash:string reason:flags.0?string = auth.SentCode;
auth.cancelCode#1f040578 phone_number:string phone_code_hash:string = Bool;

account.getPassword#548a30f5 = account.Password;

users.getUsers#0d91a548 id:Vector<InputUser> = Vector<User>;

upload.saveFilePart#b304a621 file_id:long file_part:int bytes:bytes = Bool;
upload.saveBigFilePart#de7b673d file_id:long file_part:int file_total_parts:int bytes:bytes = Bool;
upload.getFile#be5335be flags:# precise:flags.0?true cdn_supported:flags.1?true location:InputFileLocation offset:long limit:int = upload.File;

// Bản messages.sendMedia đã được đối chiếu (#id khớp CRC32 của khai báo).
messages.sendMedia#e25ff8e0 flags:# silent:flags.5?true background:flags.6?true clear_draft:flags.7?true peer:InputPeer reply_to_msg_id:flags.0?int media:InputMedia message:string random_id:long reply_markup:flags.2?ReplyMarkup entities:flags.3?Vector<MessageEntity> schedule_date:flags.10?int send_as:flags.13?InputPeer = Updates;

messages.deleteMessages#e58e95d2 flags:# revoke:flags.0?true id:Vector<int> = messages.AffectedMessages;
messages.getDialogs flags:# exclude_pinned:flags.0?true folder_id:flags.1?int offset_date:int offset_id:int offset_peer:InputPeer limit:int hash:long = messages.Dialogs;

channels.getChannels#0a7f6bbb id:Vector<InputChannel> = messages.Chats;
channels.getMessages#ad8c9a23 channel:InputChannel id:Vector<InputMessage> = messages.Messages;
channels.deleteMessages#84c1fd4e channel:InputChannel id:Vector<int> = messages.AffectedMessages;

contacts.resolveUsername#f93ccba3 username:string = contacts.ResolvedPeer;
