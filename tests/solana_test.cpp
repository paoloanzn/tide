// Solana primitives checked against vectors generated with solders 0.29
// (Python bindings of the Solana Rust SDK). See the generator recipe at the
// bottom of this file.
#include "solana.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ids = tide::sol::ids;
using tide::sol::anchor_discriminator;
using tide::sol::as_bytes;
using tide::sol::associated_token_address;
using tide::sol::base58_decode;
using tide::sol::base58_encode;
using tide::sol::base64_decode;
using tide::sol::base64_encode;
using tide::sol::BorshReader;
using tide::sol::BorshWriter;
using tide::sol::build_transaction;
using tide::sol::Bytes;
using tide::sol::ByteView;
using tide::sol::create_program_address;
using tide::sol::decode_mint;
using tide::sol::decode_token_account;
using tide::sol::DecodeError;
using tide::sol::encode_lookup_table;
using tide::sol::encode_mint;
using tide::sol::encode_token_account;
using tide::sol::ExtensionType;
using tide::sol::find_program_address;
using tide::sol::Hash;
using tide::sol::hex_decode;
using tide::sol::hex_encode;
using tide::sol::Instruction;
using tide::sol::is_on_curve;
using tide::sol::Keypair;
using tide::sol::lookup_table_meta_len;
using tide::sol::LookupTableLayout;
using tide::sol::mint_base_len;
using tide::sol::MintLayout;
using tide::sol::Pubkey;
using tide::sol::pubkey_const;
using tide::sol::rent_exempt_minimum;
using tide::sol::sha256;
using tide::sol::spl_transfer_data;
using tide::sol::token_account_base_len;
using tide::sol::TokenAccountLayout;
using tide::sol::TokenAccountState;

namespace {

// clang-tidy's optional-access analysis cannot see that a preceding REQUIRE aborts the test, so values the test has
// already required are unwrapped through this explicit check (an empty optional fails the test with this message).
// Taking an lvalue keeps the returned reference from outliving a temporary optional.
template <class Optional>
decltype(auto) must(Optional& opt) {
    if (!opt) throw std::logic_error("expected a value, got an empty optional");
    return *opt;
}

// A temporary optional is unwrapped by value.
template <class T>
T must(std::optional<T>&& opt) {
    if (!opt) throw std::logic_error("expected a value, got an empty optional");
    return *std::move(opt);
}

// Decodes a hex test vector; a malformed vector is a bug in this file, not in the code under test.
Bytes hex(std::string_view s) {
    auto bytes = hex_decode(s);
    if (!bytes) throw std::invalid_argument("malformed hex test vector");
    return *std::move(bytes);
}

// {hex, on_curve} generated with solders 0.29.0
constexpr auto curve_vectors = std::to_array<std::pair<std::string_view, bool>>({
    {"df299516b500f8bd486b00b46686c8c5229a94539ffff63664c0890934d1ef4f", false},
    {"1676e820cd6100968f689719f2af91083c36612aa3ad411cbfbdc30dc489ac2f", false},
    {"7b90b08d7676d79df0ce8c4768991529b25d908dde3b6b99f2ef900163e935a8", false},
    {"e2e7256451e07e1205f11ca96ef1c61b56e9c9eaa5ecb24d98c44bff08b5b5a5", true},
    {"f6866ca10994a5c324e8cdaf99a389f4a83883022861c615063248822d7ea7e8", true},
    {"4bf7fe7a76a5a9a514bffb0f92eeb3f2ddd8bd19f7ad4a3764b2c97f6ea5d4a4", true},
    {"785e43a85294d8ce1d8c1ffc7434c24e0d02613b7371030e4065b2b3ae481b61", false},
    {"c3b721b14c56a235bde44cbf1cac02cd3983c911538af7a3523bab1d3aa33fe7", false},
    {"4af11568fb0ce1971db851c4b730ef1947779e7fdd529231651fdb8c80ae1b88", true},
    {"2e2ea0ea5ae660156e1c7f46920a9639ca2b5ed23ec16c53c9869bf12fef71d0", true},
    {"e20562d9d4edea587b4ba7e46dc9964bf93ac19f19532b55248e3c14ec990b5f", true},
    {"b6e1e95b8b0e508a802fee67e53bc21371ce9968918df0eb5b93cb947eeb4281", true},
    {"15708589619ad5ff93eefdb6f7d092ea21b2ecc141698a631cffdd8fabb00f73", true},
    {"dbabc3dbb0750432d01728a145405cd456c9f0c9168d6d955584fd48d0440a79", false},
    {"f6a5d075c6908065f76f62a50241a24e6c2fd09cf1f049120c01b482569fe27e", true},
    {"05627e036389c9fbc11eb64bf4bbca553410a8ad876511e36adf5762388d02c7", false},
    {"528a7a67b722cc96df94b06d8a959e334a154ce6d3dff31f5caeccc9de52289b", false},
    {"df4ae98ad2d57485b446d078a33c30a95126df9d98758cf5e3f286f89ea60a02", false},
    {"6ad336d248c38b6eee0c5a5cdf67d792a9662d59059538cb2a003219fa814087", true},
    {"571829ad9f9d67651f7c7206057f1d75a21ccc7088358dd4d06ceedbf7864da3", true},
    {"9c02c1157c4cbb8a96d93f23930665364368ffa1f0806c6a95590daedc107d29", false},
    {"4ee39061aca3e41f9ada0adc21b2d0de55e7747639495933d6718e21be49cef5", true},
    {"6a8b1efe18c10dc3df6b517e1ac3242540351ba4722bb3be0b6cc9e65626758e", false},
    {"946f114bc6f9e1bff73998f97aa7e0d3354a0498126017625333f4c2d9f6b6c2", false},
    {"d4c15f2b3019ead978db371f45746372600b3211b0c45fde834920251f8c9c4f", false},
    {"2ef94db8fefb9c138da10038afc1e65f302def3756c609112b0807aeb1894128", true},
    {"5873c38e1a122f841fa6e1bfd56f085f44df5debf928e600e82e4efe913227b0", false},
    {"0bed618466fd51fb1cb8065faf519c67193ed64d183d4b6f57bb50553e6189e8", true},
    {"c62f62d1cf6932f6b600691f4239b8f14838fec1f772b103c62e78a60c752d9e", false},
    {"212af358c856f3fe2ac3af5321ecc6e8ae7e8b8d8c5ab2c4929daacd182ff3c4", false},
    {"f098dcc11efe5ecdd72894219e6a17cb2fc3a3bdcfab1131ddc38fb78d7740d3", false},
    {"bfd22034ad54d6e9f9a0d45f52531c1e302acb269ba26708665965acbca06d72", true},
    {"9a735daf0a4ece9cdfa7578b86738958a2b9e1815e520b47cf882dc3b800ad5c", true},
    {"d41c4a65d3baa3206e84dd19bf38bf9c6a110be27a2d7352c78466aded412be4", false},
    {"45d962e66b99209da24502068fd902b5a03526647b449c3f8bf459a02980cee1", false},
    {"7827b986ad28811cf581d003f0c5817dfa782bc4f85a68d8726876101fd255d1", false},
    {"2754ab6d35c922c760a90e4e6e8f799eccd1c729c019b110eb9ae0fb3a03bf8f", true},
    {"3befbaf90629e05df2ff494481958579da5cbf450e5549a94d75071068c66a42", false},
    {"a6650ec6a951dbd74a2c07b46ab89edcf8ecdf7a980a4b1517841be702e1d9f4", false},
    {"2fb7a5359a77f25a2e748c2441f3a93fa8db98837183cb9449bbd279953a273b", false},
    {"4c27f6941ea59a05735fd583fa9c1989720589f392c12a8c7f42424e799870d7", true},
    {"492f949dbd991ef636c52358e776a39dbe9712f146409c6cbd78244e24db2792", true},
    {"72c2cbecabb38d0938695e4662fd2cfc582a15f39ada7693ebce4ddf90d375c3", true},
    {"200ec7443bbcbf92d2b3b97e52eff75a025a593097362dd6c37970e841bd04b5", true},
    {"fa3569de91af7d9661e4bbe0c9bf9b3da89635e1142bf752af475da3cbd7bd7c", true},
    {"dfaffaa5add86e70577f8c5cbb47f88f8799ec07a867ac46b200533728033fac", true},
    {"fbe2a80b92cbe31d5bf8ec32eeef0939d882e91e09de9fd3f85b8958a1b6e521", false},
    {"91277988fec4a2e629f5a84d5062e328f019156c40ef58f8efffd727318bd15d", true},
    {"ec65424c9bc97163d0daba9f5e8d473294a6c40bbd7da4ef1ca3c7460f63b19a", false},
    {"7c02a690f02c5cb655607d8ff286a394be6574d5e83c60bba8bd426bf2740ab0", false},
    {"2e473fad5c0accca8deda7514e467ed1a778a3fa95386e1e86a4d26fae192bcf", true},
    {"65cd80647e51fd2ad52335922dc26db140b42a9c0c7c347b9b3c019a75abf38c", false},
    {"5c1ef422fe602a8c58ce95e44770580045b1ba4bdeaa53f8e43eaf4722475037", false},
    {"d4e2c0dccb994579d93deaf77b500f70a0d08365a7deeaf08670bc15d2497251", true},
    {"fef745bb4725ee38308eaadda5d954f805f8022bf914e15ad3c8a7e0120deed7", false},
    {"e13a26a7b753f4a48bafe3bb681da4a7a9918e7c868fd253075f3af59e7f6e3f", false},
    {"d25274a3f3f17a194c14184cfb0a9845899edc9abf1bdf95e85857efe1d1d621", true},
    {"ddce9ace7491d0ccc98c88c7750c2a2f8b33bca1640cb19960810ac4efd25ff0", true},
    {"65c5720cfa37386fd55edca5ecb433a14f87177c45d35a0405a52432430e6ea5", true},
    {"70da673741bf13ab478f77584f64b510c48e065b569b21d5021355fc5ad18f03", false},
    {"9c552ba792ee4e61dfd5cc2f4c8612fc75e14bf10531e1d66886e017e60dcce8", false},
    {"fdcfdf6f3b1cb7136b2fd89c44f2a5aa8ac2dbd6016253bbe9d421ef44c62ff8", false},
    {"2d09068f285cd853efaddac98f03cb27420943582f6a983894baf2ac471393d8", false},
    {"30566818ccb040cae9e9be38f7ef4e5d88797d316ceb291e599cd381eda1306c", false},
    {"2ee40876772abccace87b699e6da85c61e5b36402a8f7d00b3b1a035360742be", true},
    {"de12df788bb29e34756e4a187f36ea0c47c2afdc4d8699ae60648950040702cf", true},
    {"c810aaba52540abbf9f024d7f10116583ddc5dce9a2a0069412c00339ee15963", false},
    {"a3ceadef5d53e3362742b0c151f27725874f5b3f604de3066abe0fa1bea224d3", false},
    {"3e1e56a4e9bbc616047f970b94446b00107e314cb1ddfaee5f3e9ca7ad2a040c", true},
    {"dde2824f575369b9aee9b342d70b36e81f60d7d523729f137180b47c38c1b269", false},
    {"3b22aafa63ec0e27801b5739499816c0126adb56662b2ee30d6c3a9cad026704", false},
    {"1842c7e3a89ff962ce823b43d73cf66e09edbe77dcb46fdb0052f396ce87dc76", true},
    {"f7e6e07b99b2bdfa4c734fe1ee9499ab51f57ed1fa09733dc66d954e6da53e03", true},
    {"9220a16972dcb7876df060a3d20c0f284b516aadaece195e151d2315ff07bc00", true},
    {"6a6de811c90fe13165a7d10b4ab6e966b37eef0d446465f811d778bf0e19b175", true},
    {"3c9a190563a075e4c814e5221d8c775aba453c9c384734bfc89d61baa263e466", false},
    {"c645d4e02c513b1e643ebe6c388a47ee8bc13632e0f1df959c89b914b392bc03", false},
    {"8421b24a72e995e36be216fd3f4f9634682be07529a064815e4be34fe835692e", true},
    {"b08f72f93b511fa9e43086a136274a3ad8827e5520934dfa172b4a4ee679d100", true},
    {"3e0f71dace264716e6e79149219ea50a1a65c19773f73c939436698677636816", true},
    {"297b245f869a500b8107a28ba29cdc5133487b7ca35f2a6a55d4cb5ea648fad8", false},
    {"420d7dc8263309e4955864bd719a0f44615a7c4257e2240794b66c76b1a3e316", false},
    {"305160e9dfbc05f3bd26509b712f55b9642c9d3e1f36aa63154229a5ec00d031", true},
    {"c11d762155c5205e911b70affef1abe960ebbe63a971b47c8e3b86dc0f2cb8a6", true},
    {"adf52cd4df298747863b511f9544f4ba6e18a7b2d4dafcd1e2ae4e6010ac606a", false},
    {"177d5d2c981c7de1eb2b274b02ee9c3a532bbe77995a45f81ca77ffa750ecb1d", true},
    {"2445b274c37710ae5943b02228319c89f91c903b17f955548e67a87df13ada89", false},
    {"88c23dcaa757042f994ca1fe15b4f546fe878c12f09c75289c8e89c8a78af152", false},
    {"f6802540bfb6bb42c607ac58df60b6beeb17395e0e530cd346af14ecd8ca3c0f", false},
    {"6d6a31fc18acee2e07d7c3457f2abf8da3ed7539acd242bbeb28752ba1907c93", true},
    {"dd602dd24ba68fcaae4c67e274b07dad9e231be81bc0c9f383c08cbea4e3994d", false},
    {"4096c5707f63005bb0ce8787c732ae5cead0f57473c38f7dcabc2d2d7f8aa377", false},
    {"2a32db904275da011a3cfa193956be9b3b82a71671c508907dde2ee29b43cdfe", false},
    {"1ba508f4d628b9091fe6d2fba5a92f7ab652642aa4ddbdb0601bad18a12fc6d1", false},
    {"800bd424c98a5c305cb6467a51ad5836c68913d55379f34a8208dd9be23f311d", true},
    {"7438bee6c36f77488d83852288744ee4a8b71cc86dee14700fdb6c0f7a894ebb", true},
    {"cdf0d60fa019c03f3327c18e74e089b137ecc8e9256646fe9a6fa0e4cf81cdb4", true},
    {"b6a9010812f3568a8a52523590a9ab56f93701c7a37805f836da576efa165303", true},
    {"f49f44bf8fecdac57fd3abb3c30e34987986e706b8be7f87a85153b879352f30", true},
    {"0f38cc44ff1bdcb521a248476e255cd2c150eefeb0ac92edb16cdfd7a0fd56cf", false},
    {"423a7fb6af028208336d781ce75c9890aacb56420bf50e776069923713d06aa8", true},
    {"57eff3ac30b45dc05fce0bd182973be9992926dc6d5e8bb49c85daa33915df68", false},
    {"17691fe23f9a60fb498949b0dfd1796433f4a53cbec3291028e7ec701d23203a", false},
    {"01525852f6aba362e095615e2aaa81533f68ab8663ea1585bb5ef5328a55a29a", false},
    {"d8d00e798954ba84a92d97976a0db534c7fa7ea7bec4a4e7eb7d9a0059ae5592", false},
    {"774e51a19d9644b115c2fa10c7a6fba9e854b8c0093f09da986267b1c7177042", false},
    {"f65ed6cffe76b433ac3a09897f57bc9de1b2517edcdd80a76bbc0e240144ad7a", false},
    {"f5373f0d783a59ba3423b704a06fa2339563ab6486c44a7129f8dd957e14890c", false},
    {"6b84b0270cbebbb684f8060db2f130e3de007a42d82ddd3a1ba981ee4e6fa02b", true},
    {"826f94f189f72e74a6bfcdad0c80a428ddc2307fe6d233d053bc9b3b20cfc655", false},
    {"175dc0eea0b0a123d50a7580d02db241370b64179cea188b6cb31a275f106ad9", true},
    {"70d7eceb3e32e6a68ac4d61d478c27ee489b9f90b4c7ebca8bfb9079bc41d5f4", false},
    {"b09abae91fb6d926b363e0523b4af6a374337cb57ea9c16dff0b9b96163160e3", false},
    {"b29bc06a6758a4e6b2b28105b1eaa3b6c3a9ee9c904212ca4d9ec1449ab66b1a", true},
    {"b5ac79c2d8c2b67d97f11ee58f2d8a65b25b303bc5576395b20058f9db10f9b2", true},
    {"38070c1ac604996d1676e6976769f4afecb2d2ce8e898817a6f6ee40389e0352", true},
    {"9644de778a6ad4c8c6f80dabb1ef308cbcee241d4ac6c95d3bd0ef961fea09c8", true},
    {"f137fec1007edec7e3229b21ff1064c032efc9ce8b6350e9933e314b308c9f96", true},
    {"58ab2faaaceecffddf734390c7e089ad16141e8b3c77d3d6ce172a19f2965533", true},
    {"bef88423ee61174dc4d9c876523e4d78cc9ce36c62ffb9ec93b5c64ee674046b", false},
    {"57bcc77e185a6dcd5f30a3ff158d5efed32c1918c99d91fa9f6c6f831e0610ca", false},
    {"62d1f3d683e51650a6d01aabc79e4b36f5e4d40900d70fb98ddd640411e5b3d7", false},
    {"e179b3a38cea84033e7bd47287c21d9615bed2c55741489d26fe1503f07876d9", true},
    {"3f2e42417364cb0ff859ed243cbf8ce7c0a0471c666f81db59e6a6dc0d60ba98", true},
    {"7da69e9bbfd61913bf333af67acfceba1d72cb59d598bf0a4a6a4daccae6fdfd", false},
    {"801ee6092277358131c2a861dc0874c9cc8260d158a3bb68301f66a4d17e5dfd", true},
    {"3bccfc8c8392095a0e767d94dd3e88404d3bc9f418c144591f3b253bcb93323b", false},
    {"e327d59a15de0aeb5338917a4827c7e0da4a020557828e63f0d0a1d370aea0c2", true},
    {"bdde26677a89a1f9f591dcaaca74b87023a8d7273f85b7d001395241582c2704", true},
    {"3926407412e5ed01a57d9bd98b19e345a4ca4c34da67f6015fbe3e1c8acf0279", false},
    {"35b08f1698667df9f5e291e20d9ee05b32d1e038819824cf7a63690d0bf71735", false},
    {"09558d23c601da13ef01929362b6c66295bf429071ded4d76f473797c2365670", true},
    {"358bc782c9ccd1d4eaec5ccb2cd5782d5b71a4315c66f17d543012ae6d6e557f", true},
    {"6bfb98b072827156eff77420e5434921490665d040f1e769d932360ddd56ba35", false},
    {"dea75d69989667ea58efe43dde9cd1415dcc3fe2e8788ac35d57de0ce21cc0d4", true},
    {"8b659dc16fef3911c59047bf7a0e0627e8ec8965d57ac71fe91e9682023b982f", true},
    {"6d0af9cb4b3c5cc110be19d74e8dca4805936044a00bddfd35ef9e4eac594e5b", false},
    {"bf1a85932ec1a31f04bdfffda7790a8f49e600d01376f6c29ad81c86f9c13518", false},
    {"1a5c9b9f44333fd643ec4828c9a466502fdbd22c81eb4f90fd21d41c61192137", false},
    {"40274db89fe5ba377df7000853cec0c10e01a98d8dd9cda265b46f53fb0d3fae", false},
    {"bd0ec4579cac7dc4d885e4dd2b078762816afc3ac4d278240c25708757864707", true},
    {"7f75354fd570b64fb8099a75a81882d2e79e7352dd0afd4d26089484aa0286e4", false},
    {"18ba637942dc6c2cbf6178d378421d31ad5465acfbdda50811be1b19ffa3de36", true},
    {"ddbeca2a2237d7a8237610fcfeda62dbd77555f091a953fb0d751d16c4dbd1f4", false},
    {"b500c1672de4a044ee666a527489a065c31523b16d39184d5cbe5b2f5132b6f0", true},
    {"c8941c312e42b7692a4cac7d89e7e8db2291263a219039b287adf1c4211c80e9", false},
    {"65c1e1ea6eae660238ae508fa02b8027fd7ba93739ea4d7e1d699e23b8cb1765", false},
    {"9a6ad2f07c73572ed31c5a39bb6c5c202ce8b04eeab7259b8fdc1e74f3134d99", false},
    {"6eb1e657b29bc00a2373eb175fe86f25bbbe3e020b4c78d5e5037e7afb1a5811", true},
    {"17968c9c9a4a6393c45f83eba3c319dc75421cb2a19aa89aec8697e58e23cacc", false},
    {"e793ec3ef74f6a973a51ba29bfb60e8879b86374445ae6b73ffc5d2b4970ccdd", false},
    {"49aa8d40dd184f34c6287e08f960d23f8df62f33804d861d5dbca780ce25c606", true},
    {"bc45d98fd6baebc8a8b69a85d26f14104ee14d4751606e3774779bc17b41d88f", true},
    {"d630cda2bd47d28c016b2e29dd197fab216b9abc1f07c838eb10f9725dcdfe23", true},
    {"da3613b236b26f552411cd5c726e523471662c67576199377c2a86942b558b09", false},
    {"9b9047fd32370727247eb7f4ba89433dda4f9ac9c6f729b761fea4b7b4b2bf0a", true},
    {"28aa5b79180c8b7db9fc2d21a1691f88ecab4a66377a150004114a0141e0bfa5", false},
    {"9b3c9cbe848bbea223cd198ae7784497f3bddcff4b841070085b63f7e9fece6d", false},
    {"79d2bc537babc6367972365ceae1f6dba2967cd8dfe4f4de85f73d662a1cc11f", true},
    {"e6997c713f2943ca482f7a4e165328bc7f27dd0985b00c68b8b20afd5e64539a", false},
    {"e973b92a57583ddfc3ab7e50b5e382e7dc1644a3e1a255b51915b8f4df26b626", true},
    {"7eff0ec7bb76b87e21da0f5e8b1f34baea5efc9063e21d90049b3dcb621972c0", false},
    {"93e78e301f5cdd5480111e99ab665d8537d6ef5d178e9daf83519e9fe00350ec", false},
    {"66ddac4a9b1b5c855f5de0e3bed21e9141a54cdc480b03654d042d1abb5757cb", true},
    {"6957d63a12d96319e855bcfecadf2b7c976953f593bcc25a65dd0648b38a6994", false},
    {"f49b51cf735d4e53e9c55f10b098e5dd58325d293d25594836b92464fb2dda0d", false},
    {"ad9b12c671d5955b4c23bfa6ea49e4b9631647a10c590785169050c3effb52d0", false},
    {"9bf4487335528b5f4daaefea6236562c1003388c5de1dfad7aef9728fb3b61d3", false},
    {"79800bf67d9ca926caee06b71e4a15a104096c507ee45eafff8407bc82ea171c", false},
    {"c0d20288c68366549e191c4ead107f685412a19b3d042fc0cae899fd9af707b1", true},
    {"03cceb548f2149a46020e832b550aee08fae3d9ffab1ec2b6f443263034e0a97", true},
    {"7a4034213e10a0932ddf648ef557c106d1a6f31ba2fb921908691e850b9ce812", true},
    {"2365ee1dd64ea6c8ed226b3b9a40806c9419b462b27fcce7f2ce2af03a1db998", false},
    {"7a49c48661d17a9617cf3a54ccd53d24df606bf9a0685e810bb48148d060ec8b", true},
    {"6b70f6b70375f4c685c671cebf51a67b1c8190bcbf84de3875e12a2ed1fc8999", false},
    {"fa27c9b51794aa34008cc99da5e6f47b4c291778f1661c47c2a0e352727acb45", true},
    {"da02e9c0dc1d1b867130aeae083b9b3715c7394444252a97f09a5d8c0965545a", true},
    {"9c8b0a8588184f2f1c29311be2f90a186fd1ef8a9d4166d1184fa45947d9c3cf", true},
    {"715b3c556df2e528456aff4f9051c4ab274b5d8ed2c5a33a0f2b200a6a976b6d", false},
    {"d2585c94a41398bf2510827a278752717f0591fc60a1f6d0075b1233bdc8fdcd", true},
    {"640693f6d3184c6c1497b51858622c6337e4d2924226ded62afc2b72725bae74", true},
    {"86d72682b1238a5eb8106bd535ea3cf6192013d08a5cb0c666bca94d80f7db60", true},
    {"98f5693717dc3a35fd85ff0f0454cb9ed986f018b19182cb14896b6f49043a98", true},
    {"c71da2ddf8cde09cf1f298f917387b709487fbc5d693dfe3d713e4581efa1d74", true},
    {"6b3a699624f0e37bf1c163d267337d8d0b541d8714e577cc9931134910d45cfa", true},
    {"14ddfacef9ddaac5e56db52f9ebd12d5601b4753c8e62b07abe7a484a1781a6e", true},
    {"3f5baae20bd72bd9d6a0ccc361d7d80b68d9b74b1eda622ae5078361d7844d86", true},
    {"d089df196199cd1f9f059e983c4dd0718dc21e434f5abda3dfafda518813adc7", false},
    {"e033f4a180ece507046179e66f4b61ac1ffd21b4e6c50c6404796124e40cfc9e", false},
    {"df43bad5ded739ba6ec255fc1d72f74dbd01301286612c1d286aa4cc36af907e", true},
    {"74f87ba8cf72358959e2cade025462b6c4f73bb9aeb37f2a5252a8ba5de86978", false},
    {"4e2ae865d5383c3d61cc9b212281ef921848dfc3673f9c679f4042f20114be35", true},
    {"c85b41aa046df5c1e0457b16deeec0a27a485f040e4b1c6fd7493dbb9bee6260", false},
    {"32137ccbe295525eb546871f60634adb475fa19fafe6caf4ccdfa2de7efaebe3", true},
    {"89136d37e16671ace723d42d9b9821849d7bae00f19621cda6540c8a9b77edf9", false},
    {"5b13c928203341cea95dec996c0eac1663ed79515423c346b5bde87e27319f1a", false},
    {"0b02f23da2cfc86affa9b5eb68b508b6a94e94049da2001b38ce4d740255568e", false},
    {"0e4cd8cab4435770882658988d5fc0d30953bb92a0d32c4bdd88288e0aafed12", true},
    {"a89204fd4aa068c062594eec740def26e1727704c9d8ba302b89ec6f85789a3a", false},
    {"9523c3679667425eb0899200d5343b363f4552fd5250e9c003cf4641b0d576c9", true},
    {"2f8179ce3621d45c81d6f03d984790fa61bbce23d2d52e755e72cf93716ae3e3", false},
    {"9bc334a3ab89deb11ed2835d1ba8c75055833c7d827087213027633fbec53995", false},
    {"f3523db4e32ad355964149eece60e0c23c289792c2d32fb542737e05215de4d2", false},
    {"c4ff497aa9cc980986f116bbfb8385ecfd9ee909daac70f4c96da362df27e055", true},
    {"2972306444cd39249c0e113c8d9b22b72f4260d6f6f7730c4f75babdde9d6631", true},
    {"124786f11dd05ee8efd143e5f18b14a9fabf01e9d3c57f688d7d8147f5688154", true},
    {"cc50f59ef1c30cbea22d9370acf113f76df5647d6a94efa3b32eb59b58d70b8d", false},
    {"00923a524030a3b79f630e7395d1cf714235a5b5adfb2219787b314eead193dd", false},
    {"88e82276459b595ea370847dce07746eaebcb6732c0d0f8cfc893d0c3f956333", true},
    {"9703b412adf5465e418dd1612353f151469758179735ff8aa723d449ae71c913", true},
    {"499ee1f05f5563fb7e2f722a7f61c567d030691af3cda2f25d11411ba6601f32", false},
    {"b66c63ceea0ab75572ceb3f48f64d9aad2e8d22a87ec973f8bed80ad00e97c09", true},
    {"f3cd93c4236fc63d6467c446b3e35ebc4422fef3642ce69d952d757008a6258e", true},
    {"e97f42598076b8069cd7110d43ee154a920557665ec2c4985ff71847ff327797", false},
    {"2cce49d3a0690926d59f0e166e01a991ee9dd0675b430c85ad7d921daefbd96a", true},
    {"1d292d2aaa49ab4b5c13df7e179c0aacfac77563bc94e22b8074a3bc6f2148a8", false},
    {"1446090878519eff88089abd58e7536785aa6a664ce315941fc5ac05d9b57a85", false},
    {"f520e61481b42ce2dfc3164b5e63d8c444c003530846eb214920ec7c2d6e4692", false},
    {"e982c43e70e55e8349a8fb43defecc6d3bbd525f49050866174da371d2a5fdc2", true},
    {"3145099f485b1d5b2db1e533c76f5e1754a6227405c0fe15c67b1fdedfb0e1ee", false},
    {"b65fe58b0f168ccf117f486c0486f9f5c7e4505ef605e0b01d394b1ea2be0a6c", false},
    {"4e0c864790b82d3c2d096d51ff36f647f01fc820c740cc413b1d92c026ccb145", false},
    {"8bffaa6c468e6a7a0d8b7a94d0d68db9fa5149910e618ef287af668516093dca", false},
    {"75a4406e3eebeb8053a7d75a8a1bf2f0d49b2253fa2c21787bb0a843895f5a50", true},
    {"98a829e82aff8cbae40b618d606027a3302838b0120331fb72301fed153fb87a", false},
    {"339265e07bb9e241a83284bc619a9afaad9df8ab490ce1cc73b548108564d58d", false},
    {"c77d2e96fe7d8af83d293f414b7ece43d3be0ab08f7a4c5365e063165b0dfdad", true},
    {"4c8bd893d9161e6f6b2322a8b4765a898b3fdc8f3fa9dae7824b19c0689b517c", false},
    {"665364bd85f90d2673a62bf042189fe75779af13f879aabddcfbd3a67e2113d7", false},
    {"97da6db00b3dc55e7c1de808603ac960e0fcc0c2cfad69077d003bfc49db5c19", false},
    {"788499de290e59451dc75033a1d8521a3a8ba1da4e94c3876d21c586c3056966", true},
    {"54120ffd791d6d1c45be084a81511ebf0a8600a657c96c7991d11116807127b6", false},
    {"3224ea8abf076e95778d738c0e470a854bc4128df2b2bf5abfb5b51e714fb2ca", true},
    {"b80b55b47bf2e099a2c4529132722635cc5b4afe2b92649b1f17af101412d435", false},
    {"3ff1bebc7228721e71bb6032fe6a932f2bc3d3277c1da586fe8df247f9d126ef", false},
    {"7fc0f1514cc89bc7f76ed21e6a291c084784d9656a3ac6b02bd729091113043e", true},
    {"9bced0c5588ad1d0b98408e5f342957d2a5bae1aad63102c90f7ab0aeb420bbd", false},
    {"115aef1bc93b0ca924d15b5adbdbc091f3d0f14b4d709624633caf82b6e3eaa0", false},
    {"89835ba6a9e7472431e0f833ce98c7c6308463a330910b8bde26487aee8aa2de", false},
    {"938a77167b5847e41f9cf4f9b96766dcc542b86b9ff6ffb46b93b507895e6924", true},
    {"fd63c2173f2f52c79fd94ae17d269a850bd7e6cff23d9b7f5c9537709f366448", true},
    {"bf01af5b26faae7c2ca603f28b5cdd024b1eef787daadc574b97359c86233cf7", false},
    {"828b43e59a81cf5f279f22e58bb427fe449e32acabc1426341e4f437a776c7cb", true},
    {"82d7adf1094684075ace757a91861809c88145ce9fce831976ee3e6d160999b8", true},
    {"f895286008c7821255e5adbee09b2654d176f5450f77ca604737b879ef85686d", false},
    {"5ce90a21dc0bd634bf1ca6f46c03b8a39584d0903e74a04176649481a7f9c75a", true},
    {"e114967afc7e8144f8ac1dc6b19f9cf40c2c77350c9885fb8a1ed3a2c4902f06", false},
    {"0de7fc5f3eb8c5052d652b47e88f17a7c2f67584d6fee027336f495caecc8fed", true},
    {"39057e05f58fbfa605fcd356f715af93eca598599057982a1296a79a0a342154", false},
    {"8bd83192b0edb9117c7ed9d932f2c5e059ade1f10af81c124b296f9fe89552cd", true},
    {"1bef078bec99dad71e888a090adc4a30c6c6e8849f983237df2e0e423382caaa", false},
    {"cd7666575c9d96e4767726ceb4899e31bee7895b1c4f310c1926198d6ebfa517", true},
    {"fdff73f4f0bcc53ac05ff06c346ee381add52f15dc25c2645809b1ed4e73887d", false},
    {"a3a11567a81438f474557c6a999efa6472d5fdb4bdd2f9bf4be17342d6e7c2e9", true},
    {"7ad586a1e1375f657696c066da62b0f4d1d878a50f8fca3a237464063f35633e", true},
    {"41a0df5d9f423fccddd178738acee14a4898e46a3f10bb43db0a6ca656961f24", true},
    {"ffcbbc27152edb6423a9356dd87cfd87858a3e0be9e352192982abdc9c1dced4", false},
    {"6b67bd9e1185a84c0c20186e364bf1f974d0740d429bcf571aa54c3e5caa977e", true},
    {"5a8996e78583704b3de65382aa89f47b11bcd5fa6eed9a01cc4e55ae567f8572", true},
    {"b60ba6e4979a8470e568fb4f1e485b98fa881c58f9c522f3ec667199ed88226b", true},
    {"54a8f23b854e8e0622aa4d1ee8d8d2e3bb2101e16cc595a991550549590a3ba0", true},
    {"8246dccd62baec9e0e6ecdd66353dea95aba3c79477a11b2640e86ef8d4386fa", false},
    {"62568aa9b21aaff48d6581ff38172715176e14402a7ea71fae79672bcffc24ec", true},
    {"7850f926d4d838a55f35162b58c3ffd987464cde3dc578670b2e29476036c388", false},
    {"50978ce245b85113433b7b306811e455cc6cce64bf006cc67d0885f78b4ce0a1", false},
    {"b5a886955ef258094254d665409594948c92f94013de801d20613c6ad8813097", true},
    {"584990da1d87808987d14c82fca53e6da8b6eec456aba758bf41bc0a1079d36c", false},
    {"39642882a68808afaf395ece46bacd0adf1099f339299923cbf42d7d6b065361", false},
    {"4aa59d1449ae926b1c09471eaafd95e7565655b6ae48f5b1ea3509de1d2a26ce", true},
    {"243189c9f99539e996cb5eff7320e200d7af4dc5e585ef1ab6eb708c3ed34b74", false},
    {"7bcaa80de8d5d87b692bfbd6955d827179b21deae48abbe917f0ebf158a17d67", false},
    {"c7dd3d3403af7b9a1ba254f8c2e6014941db6ffc47ed1e3ef01aec79cda9d3df", true},
    {"0b293b52271c20fe0b3b13a3dffde9d031818db2f88aa0a5b2c3378ddb1848ff", false},
    {"aceda785155ed2ee56e82ecbc0584202a891ff96993431a815d37c9b72eaea1e", false},
    {"53566861c8bf879d7ce4c5f2b871dd626451ec26c7de1f399e4e20d11c34b3b3", true},
    {"0074253a12d806458d31efa0a9cbbf611e03140c301854cb1f14ef2411cdf4de", false},
    {"061c14ad7b4da4a54d6f7c10a1a3483cdeb651d8ad8f496978a4ef510fa1dc54", true},
    {"911cd1a52b401e31c62607fdb79f0f10f4f54ff7c7d0ae8b7cc0339a2114b800", false},
    {"90bea4425d2de259e860183192b963ef88453bd9779cb407b8f20227c3fdcbbe", false},
    {"6015587d1e9d94f7da1f12b8eea22d30cc5025f520cbba54bf81798551f46bd8", true},
    {"9832d92b25740f959daec99a5a303c6a6dc2ab4de73e1b979f57cf6ef702d475", true},
    {"03cd4116b3d63280bf2a6ae0ae9393627f29857001be55c9574f3455f75fd9c8", false},
    {"cfbe611e1adf9ff4e3b4729b741eda75d721ae4c96de26af935b211dd3e402d4", true},
    {"df666ac088611ac067e492d8abd0d03a772e83d1da96371a1bfda5a1f252361b", true},
    {"9da57953f41c4d885f4750c9916368a6fd9f455b09f55b5634fdb7a71d756b10", false},
    {"93ff9ca53818d536d29d73e6ce732b3e9d4f2f5f357120de2e849dd388f28fb5", false},
    {"d25d2b1e2b754801700662738641f23b74f5661f855ebdfab98eed22610850e4", true},
    {"06e14ed9a2a9a11d489e3eb51efd0a60f0692de943ac7a65e5627d21ed57b5d9", false},
    {"25419d4203b5e5bd168c7dfb17de070d9280d8174213417aa3f88ec4222102bd", false},
    {"b819abe2b4a806f293556192e50899c28e817c39668e90517bada0d844769091", false},
    {"faebc7f98953febb211eb09f236a9247c9a410a400f812241552d086cdbe7e26", true},
    {"d173fa863331ee2c5d833ba52129875c9925058d6850df1973276392650fdfb2", false},
    {"8e3a4121f7db1a2c9510d23fa8a2e24ec6c7daa8bf4e53611cea504ba641548b", true},
    {"97b693febf2c0420b8588abfe83dfa02f427ab77342b94d140eb0061473ee18a", true},
    {"79d4f3ca86cfd740c44fa51c4aedff0d6e6f0413b9d6e8cc48721ca14d7d50ac", true},
    {"08993d702f867632395936ddb04bae4eb8703e7f3a2bf8c0cb95091f05f2e377", true},
    {"a9fb1da6aa06141a93fb9e028b68454b1e9a587ce41c697980514b1403a98783", false},
    {"bbca2a78bc70a156cddca3026224ab2e3c5b3f67dc5631228e8267b7d7f619c8", true},
    {"c70cb113d1bf76959258cfef9fd7f5a94425fa4f7b828ea8f77984131ac27234", true},
    {"ffffcc16992a4782e437e69245d031144371dacca4925e78b60c516a27b98220", true},
    {"6748b62726c0d051684097cb3501786570e158d816dbcd95164550bc869c0736", true},
    {"c8f9f9fbd78e9ffa616764ef5ff78ee020a6c1b2712d07fee095bf6a4f14e8b8", false},
    {"ed448d323361f38a4904fbbd4532560cb3b8864df32256b91f48a7c244d0ea1e", true},
    {"69420cd0183d71052541a52d733050d24883aa73037a7d55bede8a05236b7155", true},
    {"8aa752a1cd2a91beb606f7cc392cd230e6aeada9ecee843273824ab844adb5f2", true},
    {"0241e2994b28eb69b8248a991e8c825e2bbe98ed1a386f4e91780c5d8652adb9", true},
    {"ab3ec54238fd99cec8547eeaf1c19f0293f2ed8ba24677069775f8d49fe08acd", true},
    {"72d73bb2e68ce0a2aeeb9d57157159f334c5cb4b19038df75665ae1ae855430b", true},
    {"aef8cc8a38fac430c08083c2685b944cf297ddf90eb5e28286c446484a1a2359", false},
    {"3d18726865a661816517d2709b7980322d0fe940aa4bf42ea08c5fce7998f819", false},
    {"b7a5a61fc01eb264d2b9f2ce04b6a1d0199f190f29b304007b6b5d72c1b95328", false},
    {"979d9cd8efebf09e7bd6e1e2ed26055597977979cc7114d7109211758fc75cf0", true},
    {"188b1a0b98eea4ba4b0777533e83cc17c5b615beaea0f97f3e3054b520de2248", false},
    {"52a715fcafbe4d8cc6e386c40f17669535743be783f392c0458df2099ab21a4c", false},
    {"9732aa13b4ebd0cb677daaca3bf6ed92f3ff901a16bfc667b36fdd4c77ba1764", true},
    {"8a9c9252665b3bcc39d5cbd68942fe3cc01f3220c823ca882913c6af2c6f76aa", true},
    {"7590a854cfdeb98ec087f744b2744d5d01278a022cee82d0051c65e9cc4e4427", false},
    {"3ac102565823d1f2de50a8fdbc1c5d1001a0d0e1e40be6b18eb4da6fecd7bee3", false},
    {"4ffba7f7b76a4e380f15e0a4bfc6ed7081049944186b11e0471b6bde7e43a64a", false},
    {"e0331638d63b9f3e8377d941f03697d332b7e794bf2758f35c86c08242b87933", true},
    {"6136f8ccc5f1cb48b0548ace57fb92fbb10cefa4abbc3a2d9ce43a61883e63da", true},
    {"f29db50aba8d5b892438ecea61d7029bbb66cb62eae986ddafb6d47901b13d5f", false},
    {"9239ba404c8bc12e5447072c3c779353ccf05871cbdf04835a563ddad041fe52", true},
    {"f6eda54a12459bdd0abb76678880b953d87111eba96f72c1277565d99ed63dd2", true},
    {"218a694fd059162c47d92f15d8d247502f40201c063ec77631cd6ef643cd12ad", true},
    {"324bb2311153e67e2e175dcfb5076497c11e53c773b08b98b877c9db4373d6f1", false},
    {"cbf394facc2133b4d18bec44672a81bdcccd02358263ba25eb02997b2e28a70b", true},
    {"7531dd1464a05fcc87af43daf93457117032406640321c62abdd36228d323d6d", true},
    {"f30b0b785a996a6f405930061757e7cd23252602c088bcd2fdd938d39a6dfe1d", false},
    {"2b82fb274ad44929cd4349b5e706961a6de2be83b86a657806229753b64d25b5", false},
    {"4728151b15b9f6636367ccdc8a15e9b567c3aeee1889db65c34cbaa8bc85df3b", false},
    {"610d32815e2e35ab0dc819aa480f3d4e2dfb7862ec49a93323c636ac94a3c95d", true},
    {"4baf0faa8859cca5430bc8e3cb867f9e965d7a35306d4de0f7754b0e40b34962", true},
    {"97b434310c7596e97b629ddb036b275a4c420452aaf8b047ec1e563d243c4132", false},
    {"dcfed9fdbe02ca2c0785675534dda5b1a5b568d0b729b95234e753380ba76e47", false},
    {"b2aea8eef09a6ae3b7487963803356e3d5c14ed7bb8e4b7ae34432d8820969d0", true},
    {"1a1b5d1c0264804c3d796bc9c0285350a7584ceb5385139dda2812b149e950ef", true},
    {"9c23216601ee062a04e1459a6e17e686ce9a108c991c1a2c9df30a6de8e40cd6", false},
    {"f16438b4d7a0f9e3aa48b80cda4e076715260544253fc7e911294aca96155d81", false},
    {"33cf6d56051ba32d2ee73d14bf38d1a934c507659e441e09f6b7ff0e0c629e64", true},
    {"5c9b3275af151d9d49156cd44f262bd60ede64fe7b1ae3f09ca99a5d23a93cea", true},
    {"bfb465ab47ffd798e5fb9f9a409ea2103ea680aa79619c96e135793a4282d329", false},
    {"1c1b720967024c52ec1e1bc4f15ed3fe88361782536bcf417d9a93996b25c28c", true},
    {"8669fce5c52afe6b6339c392ec09fb6198ab0e8b61283e35822fb8d39cae4034", false},
    {"a2f0a7674c390b38b6086c9a7d0d2382526fb648dc6a0dcbe684f99b763d1d67", false},
    {"11ca7eacecdc223b891e368f8e6619141af79ff69b5d24e3bdf01730ff3ca187", false},
    {"efb3aa2ab17e52be8c30c1c5ca57453adc78a07cd9e59adc4c49b23ca522b260", true},
    {"7360b6a8328c5330809cfad40a24f21fdcb1b7fa84bbce83462dd227795eed0d", false},
    {"7ccf032b110d42893af0dafff454b94d1969f9cbb04ecf71c619d53694ed6349", false},
    {"27052712c3a60e769c8fa5de928876706dfdae706117e9ca7444ff66fcd8205b", false},
    {"4e7a1114353ac34cfab08c878e79e3ae8a9d255754c9a0b918806b892cbf8069", true},
    {"eae6c5f4c2534bbde27a533a53880070a7ca1c1f35d95a4a1e30608eee43cdf7", false},
    {"830edd69eab29e2ff2ea3c8910755bdfc4cdf2ad5a6a51f4017705ae315eb88c", true},
    {"e1fee412293ea96469b82297f0007990c554765ed06a0b6f26c01c08fa515792", true},
    {"27648919add11a20280573165cecfb28ad0eeb950beebdb2fca0df033ab0fd03", true},
    {"f6d31f9139531f0dcb7e0c1e5e8c8121eda5196eb32cb165f48566e079d40444", true},
    {"3cfc1996f8367074a43f3242fabe301504e3a2fdf5ef50fef6bc80b952ffb1b4", false},
    {"77ec3a56a38d92820533a659d1b0e32f8ede327a8d542e56ffaf5067e651ebe9", true},
    {"e16a56c09aae2a1d8a46b0832fe3674a3b8fe55ad8de21c0983c91bfa68d5f5a", true},
    {"7104cf92b3a247e8e01fb5f525faa5cfface543c77d31a7842812b9c24e74317", true},
    {"bea4d3447e97eb9af8c1e4932853a5cf2c8fa77e29002edd34cb94fa19c9fdc9", true},
    {"36d1d7b6d249b4980d8aefb97a70c51ba965be0ef6ab29a0e42c6cb0d0f57fed", true},
    {"a3d9f9b3282796041a54cd8da023a95ecaea3a054cd758aa508a4042c0ccdb8b", true},
    {"675449da65169a706506fdfe6e1b68ac5ad3b4d0bc047490e3889ea1487dbdcb", true},
    {"789796ddab403028e5ab88d65542b9f76ba690a7d586e7ebef97f1f582be4163", true},
    {"a25ffc2c5e3467cc965df6f5f4e8fea2e86846f740c7dc87bef1d358ea3e808c", false},
    {"20e9a25213fad24a28c90127a5e0bd811f43ba17de1cf62534fadcc8601e427b", true},
    {"dacece18c0cda7915daa3e88f75339fc4bda167bd48a3f6297eb3250f09f7344", false},
    {"0001da7fbca3e6e5596849ad3982b11ec931c736495ccdfdd9e4973bb9064333", true},
    {"37fc7d31cb8e32c52eb92205bb67eafc14800bcce07ecc2c2f4e7730daccfce6", true},
    {"363b741c455408742baa463dc9acb31a645bda2652e30bbfed2150eeff5a6604", false},
    {"0ef0401d859cb9785970c7fb5a1d160daf79039535798a636f586f0c4081e084", true},
    {"b7151ae45bd5c2ac7236495540613884932a75e2581801061ae56848703763d4", true},
    {"1f0d9aa1096fb299875182bb725f9403cda59b8909870bbd2ed848b8c4bb3c42", true},
    {"6fefcedcc8ee95693d3dffe786c79043c73f406da6ea77cdac83f948a1e1e1c6", true},
    {"a9078bc7c9c9f254aefd2ad9c0ab1c48535c818f072c8327cfd2f484cb686504", false},
    {"7445cd27ab1e6a56691ad6e7e0cf9e0b7f7701124fb4e12dfb833418984f0c05", false},
    {"d41a776031c9ef188ae10713724432bf829c86f5e04b8147503fa0d5ef259e87", false},
    {"7036d0721122c1cd92316fa896dbad15ccf7ffa3b6c17d532e7a832721b78ce8", true},
    {"dd849e0659ce0e1da9e9143d0dda6cdf034fd2fcacce506d6ec7aca151ec7312", false},
    {"f8d565830ba7b6624173d1cc31a73b1ba41ca4e901320ba2adb70d0c217cc4d4", false},
    {"b56f05be4af3e1410635558fdf05262105285d57dc109f0908b00bfd945da411", false},
    {"8f76de398fd7465d5a7f6db1fd8ffcd185f8165e9f5b28aa89e15fbf62411cb1", false},
    {"2a14d927294d9316575886a814b7845c999577e9217b231d80a626228bf8c4fb", false},
    {"6a23587fcd8a930b039aebe577e2a894540d1ec777af11055daa12b64e464341", true},
    {"ff6eeff749cad0eecd271dcf454dae9c21e08af67168d53169ec34261bf4f077", false},
    {"5164de296bf955658672cd486a26192038144261ea7b233a09eafc61a0ea8a9a", true},
    {"b55bd5d832588564d8236dda99ce3e4737c0b0d1a9bcf7f2b524d9f5dd73ed5a", true},
    {"a2e703698b8be2534faed481d1daca000d99df4fa41db4a87b234a96da032327", false},
    {"02a05b639309046b2f19d9d3fc113a5c14dacc70579bc690a2fb0a18e41b24e1", true},
    {"204d6aaee1669820cd8b9c06865dbdcd4311f0339438173a8f040eb3fd9ba7b0", true},
    {"ad820ee898226b652da4247c8e37d7037d11dd2f3a6cc6164c623a0e43d14801", true},
    {"c73bd92508054d73b32579a8fb078722595e07b8cdcef316b724f8b9b3eb6e87", false},
    {"a4a6c5220333e8783e26d3f5784c85a734f59d1fdf359890620cae9434c10454", true},
    {"92229a2f923eab532eaca1348084106f488b60fe154b24d8420d27bec6cdd103", false},
    {"a23447b03285c6b592d27b35006b14b1a952c090b5b740169ab5678c4f3b731b", false},
    {"231513c64207ac314c2457707fd81a60327140d473f289923520766e2f1eb8ab", true},
    {"c6887f20aa6e9b2a4cb6a32dcc63ffb64885902697eb903228d7cac4cdb6f5f2", true},
    {"022a971d328ccc4fc0d1bc926deaf4d87c4eeaf76e43ea5bcc474942e5aff50d", false},
    {"86b09837a4e4a5a19b44fa3b6f11ec7158fea9e9967481ee3321626621b7dbba", true},
});
struct PdaVector {
    std::string_view program;
    std::array<std::string_view, 3> seeds_hex;
    std::string_view address;
    int bump;
};
constexpr auto pda_vectors = std::to_array<PdaVector>({
    {"11111111111111111111111111111111", {"74696465", "30", ""}, "HzNZj8ubbPZYFRxWLM5xngtyGAiVR9zCft5BoHfdDKwq", 254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "31", "6b"},
     "8Txkm1btQmbkj1M8JbhuLGqsjyZxqAYTmUXqsoAjSZ1k",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "32", "d473"},
     "HQKkNfqa4c1KP4NchJHAnXeh5EG2SzDc9cu33KzKS1fo",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "33", "4e0740"},
     "Trp22XewfAvXidTSPbW26Zb29QKxLvr1YjjaM4FAWhf",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "34", "4b227777"},
     "qqEgHFqn3WfA3gZ5nq4donuD8VvdmgH1kLpLdyDr69A",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "35", "ef2d127de3"},
     "iE9hNhTRe8hz9ZYgKsZUbiRsUoCqN9KDfxG5AQpMoVR",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "36", "e7f6c011776e"},
     "ZjSLrb7Quuh56zjdERt2MPD8ugNE3VUeVhX7EFyJFsv",
     253},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "37", "7902699be42c8a"},
     "ZpsvXLEwJKiGLezUmpR3TFnCgCc9j7oBhtiUaAUs8Vn",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "38", "2c624232cdd22177"},
     "2xs41ss85GHRTCERXN6NnvZkkrxxXHsYVBf7SajxY9Z8",
     253},
    {"11111111111111111111111111111111",
     {"74696465", "39", "19581e27de7ced00ff"},
     "9jQX12Zgqc7Yd7NUfTZTQBipFoJUwWDurxmXakyK1Uqk",
     252},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3130", "4a44dc15364204a80fe8"},
     "AeqZSVEy7oetqiDy8991i4TVSGYv87XdCazCkojTwed4",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3131", "4fc82b26aecb47d2868c4e"},
     "HpDoaKydNDrGr2sxCuNLRPzEqF8mvwQEWtZ2sVHbsY3A",
     254},
    {"11111111111111111111111111111111",
     {"74696465", "3132", "6b51d431df5d7f141cbececc"},
     "HqsR6cMmX9V52WD9BvjGWBZcJoWsufTKmwixJX6tXHdo",
     255},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3133", "3fdba35f04dc8c462986c992bc"},
     "Ab7jGEE3NzG9f5wi6BhaW3b17GXjws5kCgUDUS7k5ACA",
     251},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3134", "8527a891e224136950ff32ca212b"},
     "2mHJokqZjrkfcfVPLxKz5McrbXxNnHLhFt7EDK3oFdF2",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3135", "e629fa6598d732768f7c726b4b6212"},
     "4PS4c5WjypMVnboSKGcrmMGpPfJRwYaEjWqLaSDod3AH",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3136", "b17ef6d19c7a5b1ee83b907c595526dc"},
     "4vSfUUy5jzaXe66iADnj3uwUNzRRfZS5cqCGyPCoDu5M",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3137", "4523540f1504cd17100c4835e85b7eefd4"},
     "9GiELBvazRrVrJCvKhUWwhpPChcH2ScrGZiDyUkueXJQ",
     254},
    {"11111111111111111111111111111111",
     {"74696465", "3138", "4ec9599fc203d176a301536c2e091a19bc85"},
     "3HtovxHbpRSB3mfTQk9Uhq1io1Lrvx5cA3YahLohGa73",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3139", "9400f1b21cb527d7fa3d3eabba93557a18ebe7"},
     "6LB1sY1JSf6RUgrNzpaNLKtwQEDTWd6nxeG2F1rEdemB",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3230", "f5ca38f748a1d6eaf726b8a42fb575c3c71f1864"},
     "BjwUnBEDdFU4CULRvJMuopyBF4BfvxsmM5rbXhg2JEhF",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3231", "6f4b6612125fb3a0daecd2799dfd6c9c299424fd92"},
     "C9sVvroYuVoUXS3FubXqMXLFQQHZLGBcjeVDKUq9h5pq",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3232", "785f3ec7eb32f30b90cd0fcf3657d388b5ff4297f2f9"},
     "9JrgjGnJg2uCAt6Zm4myQi7J52xowt7NGvJKns7Tiaau",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3233", "535fa30d7e25dd8a49f1536779734ec8286108d115da50"},
     "DP1WozrDYuFA7XNRv1QXMceXsdGdDhij7K4XQfcvPeRd",
     254},
    {"11111111111111111111111111111111",
     {"74696465", "3234", "c2356069e9d1e79ca924378153cfbbfb4d4416b1f99d41a2"},
     "3nATJFwB1FiXZbmuQ8fa5v3xFhERrNYH5G9P62x9bYgS",
     253},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3235", "b7a56873cd771f2c446d369b649430b65a756ba278ff97ec81"},
     "6wFDjUMRSZSoBTjzvMewShGWGdpmtBjsktNSA7g2DBWA",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3236", "5f9c4ab08cac7457e9111a30e4664920607ea2c115a1433d7be9"},
     "AXQ9woevT3iCmxcNTfXrQ3PjDZJGGafBEvoFdHirsY5d",
     254},
    {"11111111111111111111111111111111",
     {"74696465", "3237", "670671cd97404156226e507973f2ab8330d3022ca96e0c93bdbdb3"},
     "2cHjPnQe5WxJMJ4JKARpM8FGBHvgMkiQwBHndGBnTcDr",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3238", "59e19706d51d39f66711c2653cd7eb1291c94d9b55eb14bda74ce4dc"},
     "Fo3nT3FPmG9ED6tZ6iapGQREUCmWKTNcfPNEjsze1jkF",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3239", "35135aaa6cc23891b40cb3f378c53a17a1127210ce60e125ccf03efcfd"},
     "CCzPNxsny5NemBRV5ueCos3EyMSrAJt75Rtf3DBSaxuD",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3330", "624b60c58c9d8bfb6ff1886c2fd605d2adeb6ea4da576068201b6c6958ce"},
     "5wnVvGaAy2dyGCYCkMSLz2R7FK9MZD5mu3U1YfHZxRsa",
     255},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3331", "eb1e33e8a81b697b75855af6bfcdbcbf7cbbde9f94962ceaec1ed8af21f5a5"},
     "2VAs6XJmKLXbFSgMXtjj7e36jwShyk8yWNCwbyRVj6wb",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3332", "e29c9c180c6279b0b02abd6a1801c7c04082cf486ec027aa13515e4f3884bb6b"},
     "9husSfeqY8EHriNWJVj4vEECBTznJaJG817VTUgE1VTq",
     255},
    {"11111111111111111111111111111111", {"74696465", "3333", ""}, "SZ4WQBxtaHgKKfoUrXSL9mJ22Akc8vp9zm96QeBSaQR", 253},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3334", "86"},
     "DySMw4NLwhdxWzomwUQXKP4LvLs1i4Nm51CkXb6ptbjK",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3335", "9f14"},
     "DFrCmpHt4hwB4iUcj28n7V7dCj3e912k9qQKY9mFGKCg",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3336", "76a508"},
     "3qHsvPpEda9Hcq1XdQTjexdz7MEM7dhtjCrgn1nBSEVN",
     252},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3337", "7a61b537"},
     "2vSq8PGgdKdBqPNmYahsHeJ92GWo5huZEKvH4coWbN8f",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3338", "aea92132c4"},
     "GJa2BpshHaX1ss6mYuJURHpgDKoiNn1EiAEmXKgR4Qhz",
     251},
    {"11111111111111111111111111111111",
     {"74696465", "3339", "0b918943df09"},
     "G4DJKGoaEVpN3SwJVTZD5352cfMYjdsewbXaf2WoH8SK",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3430", "d59eced1ded07f"},
     "4Gfm3L9JG9FeioppKYH9Wjuh4SMvypNReKtULTHgCC4c",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3431", "3d914f9348c9cc0f"},
     "HpL9kt4SS7NnaLTLNDPkAcp1ruPgMgZzyQLPH93Jzde6",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3432", "73475cb40a568e8da8"},
     "DeLKkRuTf1rHUGb6neX5XH8NiH5qSb2XTBGcrqrdvrFe",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3433", "44cb730c420480a0477b"},
     "Gu83EZ64EEe7mrqFpWt1SGe4vr91HTU5iT7F2f77YB2u",
     252},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3434", "71ee45a3c0db9a9865f731"},
     "DER6yTfV81PJbNhEywhcsFEXB6vMVZZn9SJKRGWgXUER",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3435", "811786ad1ae74adfdd20dd03"},
     "F7RgJNbBNP9tR6LN2QCqmLd2xDudb469nD28wWADAYGZ",
     255},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3436", "25fc0e7096fc653718202dc30b"},
     "DCbafbWffchuVwFXDKLDGr142j89Yaz798qJLQnzhAKi",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3437", "31489056e0916d59fe3add79e63f"},
     "8vhhe4jTn3GzoQRd1iBAWAEJWPNuPsEBc1hQk2Z7mn28",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3438", "98010bd9270f9b100b6214a21754fd"},
     "9fwLTVZhP7bg2HausPiDBgUsbW1mG3TrjByS5eJiRuCB",
     255},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3439", "0e17daca5f3e175f448bacace3bc0da4"},
     "CZFomxqqeh3wLeiyBAc49CoovkuZL9qqw9nvLvBWFeib",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3530", "1a6562590ef19d1045d06c4055742d3828"},
     "AdxTbcY9gSbA5vdcdzCt2shDx3XTP9nTJmMkwJUgiNSC",
     253},
    {"11111111111111111111111111111111",
     {"74696465", "3531", "031b4af5197ec30a926f48cf40e11a7dbc47"},
     "J1WBQbJTWG88rWbgvvLA8B4JbvHWvzGVgmfiSFJgYbk6",
     255},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3532", "41cfc0d1f2d127b04555b7246d84019b4d2771"},
     "GRirwF4WHW9p4areMGqffRNXTWSwU9V3uAtzBuakTSWz",
     252},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3533", "2858dcd1057d3eae7f7d5f782167e24b61153c01"},
     "DL2gFz7iYHNTW1X36qJKqyyMHvDRDmWxqKHx3R2aqe92",
     253},
    {"11111111111111111111111111111111",
     {"74696465", "3534", "2fca346db656187102ce806ac732e06a62df0dbb28"},
     "774Juf7CkVWcBCpspTsppYdz3iaFi7K21Bvdt4iBZkw4",
     253},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3535", "02d20bbd7e394ad5999a4cebabac9619732c343a4cac"},
     "HFE9rcRZRSfSQest2uiYjfhBR6rqNXUhnCcgvrtBgrMg",
     254},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3536", "7688b6ef52555962d008fff894223582c484517cea7da4"},
     "7iBzVLEsHM2QXcK2jEKaDA26QFRwkFqZdtN5eU7ceNvk",
     255},
    {"11111111111111111111111111111111",
     {"74696465", "3537", "c837649cce43f2729138e72cc315207057ac82599a59be72"},
     "6RQp7nKFv3D4RUgzBJkM7KwPfDHB3UHXEp3Ap37c6arn",
     254},
    {"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
     {"74696465", "3538", "6208ef0f7750c111548cf90b6ea1d0d0a66f6bff40dbef07cb"},
     "8vQJhUHrfHSVQTM67c1Bpq962xpqoKWVssdfnGEc7zNd",
     255},
    {"BPFLoaderUpgradeab1e11111111111111111111111",
     {"74696465", "3539", "3e1e967e9b793e908f8eae83c74dba9bcccce6a5535b4b462bd9"},
     "9tmGLJAztrK9pGmSpgMgakmLbCgRcQ8Rh836nNQyjVqZ",
     250},
});
// create_program_address(["Talking", "Squirrels", bump], BPFLoaderUpgradeab1e...); empty = on curve
constexpr auto cpa_vectors = std::to_array<std::pair<int, std::string_view>>({
    {0, "9A25ePEdzyKzXN12A88rbNgmzHfFnMZzUJUsy84p4RNg"},
    {1, "7NK7nHWgkWG7bqwfT5LxcXVVGUh76mhp6kw3Vo2C3FFt"},
    {2, ""},
    {3, "EvJD3tpi6BxcezWqorg16HeQFNigF4zPPB6V7YKG56JA"},
    {4, ""},
    {5, ""},
    {6, ""},
    {7, "4pvcjXsgTNsb9rjypGWjntC9qf5grnAi6nP3UkSs4du2"},
    {8, ""},
    {9, ""},
    {10, "D91pXBEim4yEcouF9Z31LgUfgyMEY88jzuEGwh8jMAEq"},
    {11, "46PwdUKAmtPspATU73ka92TzN2Mp7otxBp868M7Dckba"},
    {12, ""},
    {13, "BxHMkX9Eo74ovcQhwHUDuXPkMHeq3BvAh1cKvbmU63oM"},
    {14, ""},
    {15, "84jL9N1UFahCtGexQJGaMXash7dBjBH5m93hG6XXMv21"},
    {16, "HkCNNNzj7jQWwGL1rp6oZt2nzRjamRsYQaTGzDvdnnM"},
    {17, "DZsgihw8gbDqD3Q8pCyPwybNLtuR4jXWQNDmfPKm6Mcz"},
    {18, "8D5B8u5KsLJqvWkusKauvgo4wdJuKEaJ2VK8KTCtQeHi"},
    {19, "Fqn5sm4QgjDrPaLDYYTUYobunAVseZMzxCP4qKSJ5zhM"},
    {20, ""},
    {21, "CPZBDwU219swBUGEoeWJJJ5y9eiPZ1mS7iXBhsUTjT4f"},
    {22, "G3Rr84uCdhiyYq6CPG2uPRFmhkmw6U81JYgED5x5jmev"},
    {23, ""},
    {24, ""},
    {25, "9Zptgx6NHUcdPuezs5n7sh5iudGGGsBZVYFF9FRtJVMe"},
    {26, "xKmSKDkdqsFJaqqBQ2vh2prj5vETtk2DA5w39VtyuEw"},
    {27, "R6UXCiNsBGabeqjy8jRqW4zPojFEjNg4SoZ1V3eZafQ"},
    {28, ""},
    {29, "4o8kGGiEEd5fyCnJw3KdQdsseHvS4mouPUByyPvpygHA"},
    {30, "5wxXtsQU4kxPWLMzj6QY1fGU1ELBbgEcgimp5DwDy7qx"},
    {31, "3cc75WgAZRiZUr24BuCNQEEn644f6VLjQ3k94ZkwG47r"},
    {32, "J3urJ9qjXu9aniaYwDgUW3hpnq1HFWzev7Xiu9favkmD"},
    {33, "CZuV4THzaBtcZaXKdwEie5ADzLx9mDQT837a5TLBqmiS"},
    {34, ""},
    {35, ""},
    {36, ""},
    {37, "AxFmupGjv9YYJcKr95FkEvdChn2uPZrxEAGruumxApn4"},
    {38, ""},
    {39, "39H6WQAjoV8Ad3QnXoWtnrAqXnAPSZE9CYm5pnMoNqu3"},
    {40, "FrUSRVQaiJ3tgY8SqvAPwgo89dr1qNwYNMjcrcbFLHSP"},
    {41, ""},
    {42, ""},
    {43, ""},
    {44, "2NDptJm55r5UX3aWc2xtYozr1kLhiREVFHFgHLWFFqVB"},
    {45, "6qBhje9kJ44o1xxEvhPPeFetxPCThEwUa4f8Lmdgqs4G"},
    {46, "7fcKtRQyAh4F9N2NtudUsxQXqpaCv6Jn1GrYruQpra5p"},
    {47, "8zkJmjNPYxuuMRXMhor4r6aVNVwLTnPiWr32oLqoYriB"},
    {48, "Gd5sARE4AL8oMdKnVvKicnScigUCYNddnyr4t3KA3vni"},
    {49, ""},
    {50, "8LbAGpsEeurPS4D1MZW78kt2YKzx28kSECTdYgrVV4W6"},
    {51, ""},
    {52, "5QDAP8Pn9Fw6YQnp4zmmFWajxK3w8aJt5ywhhRTU1pbs"},
    {53, "3yz6V1ffFEmx3AhecMZ8TdsDBwtGUdsafYf5BQGSn6FP"},
    {54, ""},
    {55, ""},
    {56, ""},
    {57, "B7JZjED11jp4SSQ2yfX2dq6Knr7UPZ6gjU8yijk55EvJ"},
    {58, ""},
    {59, ""},
    {60, "9Q36qLqe3GYBQTdGkHx6TPhX2aXJm3iJx8ieYGaRAKVs"},
    {61, ""},
    {62, "8B3vbLB3zB5NvtjP8WFWhq1bC7VtLTMjKWY6XyGDxk66"},
    {63, "FKFufze2eu5prfWZezoHtoyJeUnvS3jjPvoBd8nKKNbN"},
    {64, "3eaAgdVnxiUkyjugxGJrjnHDu8o9rT1E65aQDnmbsf94"},
    {65, ""},
    {66, "HrqyuZRpM24c8iFhq41vQbchUqFUtLKrR8NBYzmjYUPu"},
    {67, "5faPLzLM5bA1yzgMfugbPFUyCg9cUKV77XX4yBFZ76TH"},
    {68, ""},
    {69, "7Zkpgwryv9WpdNDLRJtuyNwjpCyJnVLxxKHLDA9n1QV4"},
    {70, "DyvHCtLgnDaLWeAxThnkP58jATxWLKTTvbbKCeegE4jV"},
    {71, ""},
    {72, "8SzumQmUpbdzZkm3fHZ9Hgitts9Kd2WeEAiHzvciQ3Z9"},
    {73, "BAe5jiCVmVU8TBMAKEV1pKaVcX9Ecd3zCQEFLrwSrwf5"},
    {74, ""},
    {75, ""},
    {76, "BspbFVLmifmkdfjcsierX6ASXh5aaU1eBMVhL3iHWCrm"},
    {77, ""},
    {78, "Fz8hwNxPkr4sHxKsp5uQZCFRsDkEnHDVRhtbhbVcFDMB"},
    {79, ""},
    {80, "3pSgq9pqQmRhrwCjbwvHGGBXY3NFwnxUW97TdHQmTWay"},
    {81, "F9WbMdLBAbmtMwYzrKsBVcz1tS9mx2qNW3bXLNaWfqhM"},
    {82, ""},
    {83, "2xEJbGMLu2dr9ZLj98LNwFR894yr382JM2BW2X98zktr"},
    {84, ""},
    {85, ""},
    {86, ""},
    {87, "76tiBv8NJaB1M4AYRe3ArWN7hPiza9XwUqBv65u2FnES"},
    {88, "JBFPJHEPuUAuA92GMeiuwJ1CVdQA4mLRtNx6f7esdScv"},
    {89, "BttedzPfyqRMCebwMAW1zGnBtUBoUtokwDJZKPXw6TWQ"},
    {90, ""},
    {91, "7h6bHB4ZvLsYndg4VHSRRtbT6GUkWG7iiMjDJzFQgGr9"},
    {92, ""},
    {93, ""},
    {94, "HAubo3ZfP5jPbXexycshWQWaAP2g8HcLET7yzAo7rwoq"},
    {95, "93oLS4xb7d2bao6vhzEYrjG2B521yqxSGtF3sieT8ryo"},
    {96, ""},
    {97, "2NgArXLyd7fs1hx3mtQdZSussWYk5Tt9SpK7DoxpLfyB"},
    {98, ""},
    {99, ""},
    {100, ""},
    {101, ""},
    {102, "7VsJzdby3UHM8NpCmxQdnF4J9QJWsSH9NkxdXax7pKyk"},
    {103, ""},
    {104, "DpHSr3aLtt83JmotRQxLJY9MkjWBmSyo2v3eSLVhFSN3"},
    {105, ""},
    {106, ""},
    {107, "2Ni8c4BGoUDoZDRuDN3RR6x7hmGY4nXjX8Z9No4fNf8y"},
    {108, "3fET8f3JWYzXW1RaBGjGFCcUTUX9qCwQUUpjqxFrKydT"},
    {109, ""},
    {110, "A7idPUtB2XTvNk872pa8aFGHoBZzghZY3UzViZj3JdPG"},
    {111, ""},
    {112, ""},
    {113, ""},
    {114, "3kH8HcVTuQB9AUC74yMiztMVCqRj8HnCmeQzWVAspj7y"},
    {115, ""},
    {116, "AQdTTAsVy1b2VLCkj1j2kb3WQV5gW7esFGFW5DnkevYR"},
    {117, ""},
    {118, ""},
    {119, "EGYrYWP3VGgsE84mopVAyKjLe9YEPP32943cZ69dnR3v"},
    {120, ""},
    {121, "Cc5cVbrk9F8dTeXZD3CM8gS7XZGjvaY7VReM3ibWnze9"},
    {122, "2ruAgqbLdtn5xPfvNHYKNHZSESLasaK9gHMxkkbVqBzm"},
    {123, ""},
    {124, "6ZSNBheQiJ4f1VWBYdUoH5YqHm9aZuuNrsERtPxZSWMn"},
    {125, "J4GcpGi5wfAQRaGaXqqo44sjyvnKTAoFTiQJgKySt9FH"},
    {126, ""},
    {127, ""},
    {128, "BUV7PUmtTCtmeFTFEhdKRX2Xmm3YptPJ5P5jpf7B5v95"},
    {129, ""},
    {130, ""},
    {131, "6iXy7aCHCfuvM9tRoMAjZdGxBbAtSQJFD79X9ixChVEx"},
    {132, ""},
    {133, "9VWnGxoSvAC2aUP9Q8EKpcYUYep79aswmdjeX5HJgGu"},
    {134, "5pnGEyCdihXRxPWiE5Edgfti4W7mRfb11EZ65EDncmZQ"},
    {135, "2T6216jMMCMkBrjnkUPKtGhekRXTjvNZ5iARrfTMc9Mb"},
    {136, "5gJneGCTVvYd7XrYWcBg4QMLcJ83gAMsYpRfdCxasQp8"},
    {137, ""},
    {138, ""},
    {139, ""},
    {140, ""},
    {141, "9ZLEprGDHSV9nGJfhTTZWMAhWMPaSEHrQzHuFaM6gxC5"},
    {142, ""},
    {143, ""},
    {144, "9EiST155L5mNtfbjXuh3VKnkD4cHnWy8WeL6Vm3w2zog"},
    {145, ""},
    {146, "6KRp2Fqyqpum9h3vFj1v3hcryr8T2Kp1txyagdFjkboZ"},
    {147, ""},
    {148, "3yUfBeqMohJJcpikPCcRMToiZZaf1r9zWH4w29hZtmyc"},
    {149, "Eqt4NZStTiTQ1GmzH9sMKwXPquEBZgw9rsW9GyGNTBKj"},
    {150, ""},
    {151, ""},
    {152, ""},
    {153, "4NVB9cLHd39VEZWyPrJkWyWSWnBwQRhTgai9QijajTi6"},
    {154, ""},
    {155, ""},
    {156, "7VokTWiL92wUEhsfnn8UGSGADVoWVwB8jp8p1c2C4FZE"},
    {157, ""},
    {158, "J1HyDkDS2nQcUZy8fV2Q3Sd6g3xqGpzwvrBf8oMFeA3U"},
    {159, ""},
    {160, ""},
    {161, "A3snV4LZCTKrmckWdmyUi8Xy3H9GBTS9te5CNsY9WXd2"},
    {162, ""},
    {163, ""},
    {164, "AxDQhf97fh5CfEjgYGYAZvf1s1zEYWx9hgM29yDgpQor"},
    {165, ""},
    {166, ""},
    {167, "2Crev4a1udLZ5MAcMeTCbcPpbUHBX3pwdmDDHcKY2r1k"},
    {168, ""},
    {169, ""},
    {170, ""},
    {171, "5qduFkkE68ggoetHtd7Bpeu2whmu7C3PUAtHaCRFMGZX"},
    {172, ""},
    {173, "CkXVwfjK3TPU7az7cKQUgrTtKNf6khTGU3wrV5KYbUmA"},
    {174, ""},
    {175, ""},
    {176, ""},
    {177, ""},
    {178, "GxSiHqvvB3VcrJ1zbbapQtcQDwHCVigZxjGUwF5PKSoi"},
    {179, "BdofeUeZpx7rdKcyaDWWYkQnb4wLbDnSdrFUWsdZcZsf"},
    {180, ""},
    {181, "Hmuddo2m3Nb44rGtXqLKLJFam9STpAvZyBdtkRsDbDMx"},
    {182, ""},
    {183, ""},
    {184, ""},
    {185, "GNAYKSifCoQptmntBvvWgbB5oSVTKXJTyrhmetVTDxPz"},
    {186, "EwYZDBzF7Jtq4kQL2fe9ugcb8gZXLfp9rwbzZEKZh2UW"},
    {187, "DybS6qNzWR2ZD66Hwg2NzPdpdrAM2QEgdPMXd9JvenJM"},
    {188, ""},
    {189, ""},
    {190, ""},
    {191, ""},
    {192, ""},
    {193, "6SQTd96oJKhkLVV93qWQgmsNjHbLErD9hDzD8reJfgd"},
    {194, "7bUc6xghsncbWRY4QnxWaU6U4ySrkAUqbutZR81QMzqS"},
    {195, ""},
    {196, ""},
    {197, ""},
    {198, "8PnbbeG8pFwCrg2yoYzkAKjAG6D7cCRzSrZUDMKkSzqy"},
    {199, ""},
    {200, "2PGV1WsiEGuEA4DpP6KDqw85ACA8o3E9CXfW4pAPZYgy"},
    {201, "DpArUwDJ1DjooM4dNsNMLCmUo645D4tuSpigYmsWi7Gs"},
    {202, "G2bEQ6zo2ndTsTXyx97MPahKeQwmuLVSxsUtQChzUHxx"},
    {203, "CsNb8HNuEXxj2HFi7SWmgh78sn83qFa53p38AAw3WARA"},
    {204, ""},
    {205, "383abcVpEtMJW72H7wag4HE3aJm7Ta7Sj2e7A1MPbMxW"},
    {206, "6h5U3xPW8kkKXxGFtQKSCu4SCDMGG4SgKi2eF5jjT5g1"},
    {207, "GJAfYH92gNoUHXfGqCnhwrt6f73MhkvFrX31dS9Fzasz"},
    {208, "6b7T6VBseR4xNhorvLJP67Qm3Zf8tHGdQYKQ6bzbXRVz"},
    {209, ""},
    {210, ""},
    {211, ""},
    {212, "8Hs7ikBJRthKpXtYgGngEpqfBzscD3ZKmaF1aerJvFe8"},
    {213, "DSCqKHh6V6QEtFafzUCJhcosnMNpaeQgWjNqnjnhe3dv"},
    {214, "AJseAGKCB3msGK14jqjdZJJtkNakmgJFPALkdC3zKZ5d"},
    {215, ""},
    {216, "7zuRP5WtkY4Zz9nvYhNaxZrzApxViD7wE1W9Eg4NEbv9"},
    {217, "HSjWMajUVu7RiEkSnEHuuzEeJMqMCeuxTDs8SbDVeYJm"},
    {218, "DeNVeGSEkaN7GAeHifR7msARwMcjbsjdrtNYd2wc6ZvE"},
    {219, ""},
    {220, ""},
    {221, ""},
    {222, "BNLJQovBHas5gGyawhsRRdYMSpLUD1sYbSV7J7S8fF6Z"},
    {223, "DY1LXpwYeS4i5HnN2fwxqQ9HKtmyk4o6a18Zc8zpK8pY"},
    {224, ""},
    {225, "Gt3emeC1XhXN2oEEBrgK6NQMaL8LwsbL5RgKEpcYJi8d"},
    {226, "81DKn2zSxFed9Xjux86wfEtquvEArLsp1hC1E5Su19D7"},
    {227, ""},
    {228, ""},
    {229, ""},
    {230, ""},
    {231, "o4NJ7qHnZb8LBcfauJXq1JvjipkSqjER4NNc2UunKks"},
    {232, ""},
    {233, ""},
    {234, ""},
    {235, "77nJDheaRpHGzQGY9H6vfScy8LLRsCKBtyfn8J2WpVML"},
    {236, "ADCTadcXGP8GiXxyy2d1t7UfZTzekBdG7VyPJsw5F7wS"},
    {237, ""},
    {238, "3k8mB4dqSVY47Cm2C5YZVAQdJ8U9y8T3AdcbnrRNpjob"},
    {239, ""},
    {240, ""},
    {241, ""},
    {242, "BBtY79bbxKYbx5TryVs99hocC3r2w7sj8oQya6fGtfCM"},
    {243, ""},
    {244, "CvcHtZV3bfwKyS7a2zWusMCnwxxjq2WYsPwX3JkkmDKp"},
    {245, "442A3DjfBLaeCJZm96mHAvNvSU3qqC1zvmVRn9HJh5MA"},
    {246, ""},
    {247, "A4ePFUveW975HB9kNm4TNvFUehrntCCEE9LEngsS8d4f"},
    {248, "BMGWtPoKYLdzkV1KxzGp7RmfXdEJx17GTHxWQkC13h43"},
    {249, "9d4Yn6iTxyDSeGYFEUpsDEsVE8Mjdj8nskH2TqwrdNcQ"},
    {250, ""},
    {251, "JA5i3hoCkkGc8z1vev6nPLFnaTZcktiSgLR31JZNjBTT"},
    {252, ""},
    {253, "BwEWVJSfwyFDpbtRhTdcYZGum1UJn2uuTWSmxCTrMTy3"},
    {254, "8UG5yBX8BiaoeKTWLrL4SAjpz3sDm26P3AEKsnxeSWeN"},
    {255, "HTqKuCuTUMwRJV4ceegG2CwYRxub4qjpj9DEg3nz1NGF"},
});
struct AtaVector {
    std::string_view owner, mint, token_program, address;
};
constexpr auto ata_vectors = std::to_array<AtaVector>({
    {"2NHp1R6aWkztMPV5oe4Y4MoAfyYbQNBSCvsHQKqbNjRr", "8DeGrBZxfCM8MmnQe83USz2cUrWeuTp8csSbeiE5VgZh",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "94DFfAyjoyMJQhNtJPMcRNkHqHHfGV5FVagEQjZq6xHX"},
    {"HZYLg7poMVQApuJpXgDQ3WJtNA41Luyu2PpEHPrAQhTY", "8omoWh9FDoNjj6x5HhvULDm1aPVQHng7LPW3FBx1XQnM",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "DMFUEnhssVCdjvFBsiBnmnAYdmpFeiUysme31ZmUzMQj"},
    {"4i2buXF67ktbbKvyVA2wuip3qrwzwiJyVNRVj1pg4Gje", "FqinZ7YgzXpSiRm8JPX1rgXYd8ZTqvXue5x6awm8RjB5",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "GmoGPCfgxsL7Ek9NZC6KHkkVRv2jRb8buGpMQLA3ZuEu"},
    {"4mBhfXnvUvT6PvCAVGZgEgtqkqW31yMfvNkgnTVEQTu8", "4QPLLomMKzTVeiCPfkJ9ux3ML6CkN5qEx9Yz8LZcLN8m",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "BQ8KGNC6fpmv93wQgUSPjeLg7aqHFLwAhoMwBh8WDiSy"},
    {"6TMGtsq9poV4qXvsU5QBRhJvYJtbsZqofbmX643jLtGu", "8qtn1tebECVwidcWdg74cDvMCPcjmiNGefz1xQzKj7HF",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "Bbk6MR5DaBGbzarMbERwzkwxQa2znX51tvdxnCyJVusz"},
    {"Eb7LPru7t3wuvurkgQg5Xjz64N7quUMcbyDbx45m339L", "HNrwMuEWocSPiG4gk6vUtztNqNbFD9A8WGGJRcPEALFb",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "A29eN3XG58abbkzkHkjozphLTAEfcsFJrJyTj3QUsXX8"},
    {"DRmSxkGbakisUjotaizaWGESoCJcwsfcNNuK8qwJxASh", "AsZ2H1GvCJPb73jKs1FmznmAzcmWzes14igWXrQ5kbSW",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "EHajrj7Qn9E9wdjR48yihgfoad2kW2AfXQYPRm9YHncG"},
    {"DRVjtpSRzBtmbYrZgTDv5G8wr538szadGkSXSFSwzbg4", "5FDHZFq8uwR5RCu6NZWstrmMX3hdegyGTKeaMFBfdNup",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "HuFF7QmpfzHcVsV58jUkJA7NEGT95SLtQTx7neHdH3JG"},
    {"DjcVX9mQ8BLaZvmWdRJiCNZPnMBYCVShLczBkgNRN189", "DrB3zwsLXZELPyEznoJgERkC3KCLKsuN9P44KnLwZSUf",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "9WJyPcHXFqayZGC1f1Ky24jbPuE7YwoDbNUYdz9cEsKC"},
    {"7caNKLZHe2G7BEwdCzJCfRkYRRk7XXYUjLQviS4SuqxW", "GFLc1PYrEf7NKPtkHsjScKtP9YQwhUwY1VCgWaXZN8Vc",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "8XVh3SnNf7L75nERug4yDu4bmabijPxspw4yDJBYRdZ7"},
    {"FX6bkREgU999hnfqUKUixgCUfTMLD9hf23TVMxoXvjG3", "5TiTFNsQatevLhRfRr8TY2JTjeNyAJ8fRZ7Ef6TcBoGD",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "7Khut2F2szek8ivxbDKMoU7JMMtLvJnDxScV98ZeaRxk"},
    {"GVk94iMREPNXR17zdkzHbx8iBVqoK9hYmjgyDAYcMBTa", "3EVeU6i36puaF7u3JViptkNqK4AdUdA9Y34rrAxnze5N",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "7j116ARM55a85F2c35BJ7tBNey9F58v2g1XY6dKCYobE"},
    {"Bp9MUi4t8cnpUA5EmvR13ePsiC9EYgAjmjVSDKGXtUPM", "Dt12V6oCS8sWMt8mroCVHxVbnV8utiABfHt4ZFabrHA8",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "HSX4SmYTZgX3NjFi8mRfXVVCnDYho6pzWL4LifiQGDx4"},
    {"6WTDxAMB2Cz9D9mQDHTg4WPJW4sUJoCTwsFJGcRNttaW", "34Q9AsACwRUyhFJKL9thP3asrtqTy15pKf3xjb1vYNMQ",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "7xsw9rCNbLcxdqsgF76yH1Z3ck5vTXQV6zGKL2Pyq6BV"},
    {"4c1zScj6gR35qAmfMH7vUbbGzVqDRmXU8774ZByNCm2W", "7FKeM13vFFzgZrLr9C7UFX3CzqxYuxzwyggGbnkFfZEG",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "DZk5n8QyVywFv55hoTQAQCjnCqU7pBgb7AThDXs1fVXB"},
    {"FzgZFaTY5SGh9sSUYCExT2bKhcDgPKZT42aKaxvqi5y4", "CWvFC222Qh5mji3rLjRgJ9whnR3Do3jqXBjCEkCsY22b",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "8nNHCFFGVMWPfTijZ7KBKbk8uksnzLTq3mQDNc2eHpeu"},
    {"CEFTRRskX9P4jcr53DwJm9AW3YotgVNtjB2X8fVB1NBG", "BwcqBgVNzk5a3LR3y8fb9RiRVaoJ8fGpUMusK5kAHSZz",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "4W4G5GywsJ3RbJjYqmwDC878hpN986u1GDPB1jF2vU5V"},
    {"AzkGJCwUxh8eq8LpMonvxZy1q2VodWjt9kFAtxi7L9Zw", "41J64CZo23o1gEoSxQW8Yt6iSzJ6uQdxQEbrvJCx5pwV",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "9hoaGT8Fc5Tvwiuig3Eogedhyx5FMQaUm1aZZCEwc1H7"},
    {"BFpsSsQzGwdQPm5M7YnKsALpM4PYfMkrAscgXXvxXjHU", "D33WonUW7GrVLcQSdsEMS6UEc6PtVFfi1DMJSknDtdhK",
     "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "8rJGf1qskw9qQ6qAShWahsEcA1SVkgv1U44Y9e1MooVW"},
    {"7ZY775rU5naf8sBjipiCGoMbDrDH43qvaZdmaEn96zXA", "4EoqWGcGiat6YUKYt1hq5iseBF8aeE8XKEUy6kpUeoBd",
     "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb", "67iRXPFho8v5PiCpsTTHzWQVTLLax42WWgq8Dv1vqxke"},
});
constexpr auto seed_vectors = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"alice", "HWrCL2DiCckyFMw4Z4BR8fWq1QHFGecjh19DReM3Epye"},
    {"bob", "CJHsd5oHaKftDqzDFiHpB3LqN82ftpdoYydM8qbEW6Zm"},
    {"admin", "E47x1E5KFevMHZsesS2TyNnk1wZBRiivHUxfAfiVsV1T"},
    {"usd", "CCrDymVfTunhmzpxP8kD9QxQ5DbB8rkd2niZCZz3FEGi"},
    {"", "7hdpVULS6vjsJhnS6gMH1SkRj1Dr6NZbB3yvwUJn8KNs"},
});
constexpr std::string_view tx_vector_hex =
    "02dfc95bf9ae459d00be84c0a2b347cba397a9cdee319300bf0d93796eda0e2ac55a2d65505160c58282ac87a25127edb8dd285ceda669014f"
    "2cb985eb77c48600130f6df5de302ba81457e8008655b72af88b8269594fb7203593c653cc4a78cd015f3ac39a234a7b619c75250a31364b3b"
    "5d7d678aacebc8947e4eee8a7fc30202000205f56075a8403e5b2952fd5cb809aac7ef63ba1861b86be9ceb4f77c8b56641459285d1c5fed7c"
    "c0a1c41573ed26f596f02a836236bc47889951bcadc900438ce8a7de821129e3d847cf9d4d53321d0c9df67cbbd6a329bde2ad49ced73a5c72"
    "38054a535a992921064d24e87160da387c7c35b5ddbc92bb81e41fa8404105448d06ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b3791"
    "3a8cf5857eff00a9395bf727f9aac5e80911591073fcf9c826f428804131ca089beba3869421749a020403020100090390d003000000000003"
    "01010568656c6c6f";

}  // namespace

TEST_CASE("base58 round-trips and rejects invalid input", "[solana]") {
    CHECK(base58_encode(Bytes{}) == "");
    CHECK(base58_encode(Bytes{0, 0, 1}) == "112");
    CHECK(base58_encode(Bytes(32, 0)) == "11111111111111111111111111111111");
    auto sys = Pubkey::from_base58("11111111111111111111111111111111");
    REQUIRE(sys);
    CHECK(must(sys) == Pubkey{});
    CHECK(ids::token_program.base58() == "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
    CHECK_FALSE(base58_decode("0OIl").has_value());
    CHECK_FALSE(Pubkey::from_base58("REPLACE_WITH_BASE58_ADDRESS").has_value());
    CHECK_FALSE(Pubkey::from_base58("1111").has_value());  // wrong length
    for (int n = 0; n < 40; ++n) {
        Bytes b;
        for (int i = 0; i < n; ++i)
            b.push_back(static_cast<std::uint8_t>((i * 37 + n) % 256 * static_cast<int>(i % 3 != 0)));
        CHECK(must(base58_decode(base58_encode(b))) == b);
    }
}

TEST_CASE("hex and base64 codecs", "[solana]") {
    CHECK(hex_encode(Bytes{0x00, 0xab, 0xff}) == "00abff");
    CHECK(must(hex_decode("0xABff")) == (Bytes{0xab, 0xff}));
    CHECK(must(hex_decode("")) == Bytes{});
    CHECK_FALSE(hex_decode("abc").has_value());
    CHECK_FALSE(hex_decode("zz").has_value());
    CHECK(base64_encode(as_bytes("hello")) == "aGVsbG8=");
    CHECK(base64_encode(as_bytes("hi!")) == "aGkh");
    CHECK(must(base64_decode("aGVsbG8=")) == Bytes(as_bytes("hello").begin(), as_bytes("hello").end()));
    CHECK_FALSE(base64_decode("a$==").has_value());
    CHECK(hex_encode(sha256(as_bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("is_on_curve matches curve25519-dalek decompression", "[solana]") {
    for (const auto& [h, expected] : curve_vectors) {
        INFO(h);
        CHECK(is_on_curve(Pubkey::from_bytes(hex(h))) == expected);
    }
}

TEST_CASE("find_program_address matches the Solana SDK", "[solana]") {
    for (const auto& v : pda_vectors) {
        std::vector<Bytes> seeds;
        seeds.reserve(v.seeds_hex.size());
        for (const auto& s : v.seeds_hex) seeds.push_back(hex(s));
        auto [addr, bump] = find_program_address(seeds, pubkey_const(v.program));
        INFO(v.address);
        CHECK(addr.base58() == v.address);
        CHECK(bump == v.bump);
    }
}

TEST_CASE("create_program_address rejects on-curve results", "[solana]") {
    const std::vector<Bytes> seeds = {Bytes(as_bytes("Talking").begin(), as_bytes("Talking").end()),
                                      Bytes(as_bytes("Squirrels").begin(), as_bytes("Squirrels").end())};
    for (const auto& [bump, expected] : cpa_vectors) {
        auto addr = create_program_address(seeds, static_cast<std::uint8_t>(bump), ids::bpf_upgradeable_loader);
        INFO("bump " << bump);
        if (expected.empty())
            CHECK_FALSE(addr.has_value());
        else {
            REQUIRE(addr.has_value());
            CHECK(must(addr).base58() == expected);
        }
    }
    CHECK_THROWS_AS(find_program_address(std::vector<Bytes>(16, Bytes{1}), ids::system_program), std::invalid_argument);
    CHECK_THROWS_AS(find_program_address({Bytes(33, 1)}, ids::system_program), std::invalid_argument);
}

TEST_CASE("associated token addresses for Token and Token-2022", "[solana]") {
    for (const auto& v : ata_vectors)
        CHECK(associated_token_address(pubkey_const(v.owner), pubkey_const(v.mint), pubkey_const(v.token_program))
                  .base58() == v.address);
}

TEST_CASE("seed() keypairs are deterministic ed25519 keys", "[solana]") {
    for (const auto& [name, pub] : seed_vectors) CHECK(Keypair::from_name(name).pubkey.base58() == pub);
    Keypair a = Keypair::from_name("alice");
    Keypair b = Keypair::from_json(a.to_json());
    CHECK(a.pubkey == b.pubkey);
    CHECK(a.secret == b.secret);
    CHECK_THROWS(Keypair::from_json("[1,2,3]"));
    std::string bad = a.to_json();
    bad[bad.size() - 2] = bad[bad.size() - 2] == '1' ? '2' : '1';  // corrupt the public half
    CHECK_THROWS(Keypair::from_json(bad));
}

TEST_CASE("legacy transactions serialize byte-for-byte like the SDK", "[solana]") {
    Keypair alice = Keypair::from_name("alice");
    Keypair bob = Keypair::from_name("bob");
    Keypair carol = Keypair::from_name("carol");
    const Hash bh = sha256(as_bytes("blockhash"));
    const Instruction transfer{ids::token_program,
                               {{bob.pubkey, false, true}, {carol.pubkey, false, true}, {alice.pubkey, true, false}},
                               spl_transfer_data(250000)};
    const Instruction memo{
        ids::memo_program, {{carol.pubkey, true, false}}, Bytes(as_bytes("hello").begin(), as_bytes("hello").end())};
    Bytes tx = build_transaction({transfer, memo}, alice.pubkey, bh, {carol, alice});
    CHECK(hex_encode(tx) == tx_vector_hex);
    CHECK_THROWS_AS(build_transaction({transfer, memo}, alice.pubkey, bh, {alice}), std::invalid_argument);
}

TEST_CASE("SPL account layouts", "[solana]") {
    MintLayout m;
    m.mint_authority = Keypair::from_name("issuer").pubkey;
    m.supply = 125'000'000;
    m.decimals = 6;
    Bytes mint = encode_mint(m);
    REQUIRE(mint.size() == mint_base_len);
    const auto dm = decode_mint(mint);
    REQUIRE(dm);
    const MintLayout& decoded_mint = must(dm);
    CHECK(decoded_mint.mint_authority == m.mint_authority);
    CHECK_FALSE(decoded_mint.freeze_authority.has_value());
    CHECK(decoded_mint.supply == 125'000'000);
    CHECK(decoded_mint.decimals == 6);

    TokenAccountLayout a;
    a.mint = Keypair::from_name("usd").pubkey;
    a.owner = Keypair::from_name("holder").pubkey;
    a.amount = 42;
    a.state = TokenAccountState::frozen;
    Bytes acc = encode_token_account(a);
    REQUIRE(acc.size() == token_account_base_len);
    const auto da = decode_token_account(acc);
    REQUIRE(da);
    const TokenAccountLayout& decoded_account = must(da);
    CHECK(decoded_account.amount == 42);
    CHECK(decoded_account.owner == a.owner);
    CHECK(decoded_account.state == TokenAccountState::frozen);

    // Token-2022: base padded to 165 bytes, account type byte, then TLV.
    m.extensions.push_back({ExtensionType::mint_close_authority, Bytes(32, 7)});
    Bytes mint22 = encode_mint(m);
    REQUIRE(mint22.size() == 165 + 1 + 4 + 32);
    CHECK(mint22[165] == 1);
    CHECK(mint22[166] == 3);
    CHECK(mint22[168] == 32);
    CHECK(must(decode_mint(mint22)).supply == 125'000'000);

    LookupTableLayout t;
    t.addresses = {a.mint, a.owner};
    Bytes lut = encode_lookup_table(t);
    CHECK(lut.size() == lookup_table_meta_len + 64);
    CHECK(lut[0] == 1);
    CHECK(std::equal(a.mint.bytes.begin(), a.mint.bytes.end(), lut.begin() + 56));

    CHECK(rent_exempt_minimum(82) == 1'461'600);
    CHECK(rent_exempt_minimum(165) == 2'039'280);
    CHECK(rent_exempt_minimum(0) == 890'880);
}

TEST_CASE("borsh writer/reader and anchor discriminators", "[solana]") {
    BorshWriter w;
    w.u8(1);
    w.u16(0x0203);
    w.u64(UINT64_MAX);
    w.string("hi");
    w.boolean(true);
    w.f64(1.5);
    Bytes b = w.take();
    BorshReader r(b);
    CHECK(r.u8() == 1);
    CHECK(r.u16() == 0x0203);
    CHECK(r.u64() == UINT64_MAX);
    CHECK(r.string() == "hi");
    CHECK(r.boolean());
    CHECK(r.f64() == 1.5);
    CHECK(r.remaining() == 0);
    CHECK_THROWS_AS(r.u8(), DecodeError);
    // sha256("global:increment")[..8]
    auto d = anchor_discriminator("global", "increment");
    CHECK(hex_encode(d) == hex_encode(ByteView(sha256(as_bytes("global:increment"))).subspan(0, 8)));
}

// clang-format off
/* Vector generator. Regenerate with:
     pip install solders==0.29.0 && python gen.py > vectors.inc
   and paste vectors.inc into the anonymous namespace above.

import hashlib
from solders.pubkey import Pubkey
from solders.keypair import Keypair
from solders.instruction import Instruction, AccountMeta
from solders.message import Message
from solders.transaction import Transaction
from solders.hash import Hash

out = []
# is_on_curve: deterministic pseudo-random 32-byte strings
curve = []
for i in range(400):
    b = hashlib.sha256(b"curve" + str(i).encode()).digest()
    curve.append((b.hex(), Pubkey(b).is_on_curve()))
out.append("// {hex, on_curve} generated with solders %s" % "0.29.0")
out.append("constexpr auto curve_vectors = std::to_array<std::pair<std::string_view, bool>>({")
out += ['    {"%s", %s},' % (h, "true" if c else "false") for h, c in curve]
out.append("});")

programs = [Pubkey.from_string("11111111111111111111111111111111"),
            Pubkey.from_string("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"),
            Pubkey.from_string("BPFLoaderUpgradeab1e11111111111111111111111")]
pdas = []
for i in range(60):
    prog = programs[i % 3]
    seeds = [b"tide", str(i).encode(), hashlib.sha256(str(i).encode()).digest()[: i % 33]]
    addr, bump = Pubkey.find_program_address(seeds, prog)
    pdas.append((str(prog), [s.hex() for s in seeds], str(addr), bump))
out.append("struct PdaVector { std::string_view program; std::array<std::string_view, 3> seeds_hex; "
           "std::string_view address; int bump; };")
out.append("constexpr auto pda_vectors = std::to_array<PdaVector>({")
for p, s, a, b in pdas:
    out.append('    {"%s", {%s}, "%s", %d},' % (p, ", ".join('"%s"' % x for x in s), a, b))
out.append("});")

# create_program_address with explicit bumps, including on-curve failures
cpa = []
prog = programs[2]
for bump in range(256):
    seeds = [b"Talking", b"Squirrels"]
    try:
        a = str(Pubkey.create_program_address(seeds + [bytes([bump])], prog))
    except Exception:
        a = ""
    cpa.append((bump, a))
out.append("// create_program_address([\"Talking\", \"Squirrels\", bump], BPFLoaderUpgradeab1e...); empty = on curve")
out.append("constexpr auto cpa_vectors = std::to_array<std::pair<int, std::string_view>>({")
out += ['    {%d, "%s"},' % (b, a) for b, a in cpa]
out.append("});")

ata_prog = Pubkey.from_string("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL")
tok = programs[1]
tok22 = Pubkey.from_string("TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb")
atas = []
for i in range(20):
    owner = Keypair.from_seed(hashlib.sha256(b"owner" + bytes([i])).digest()).pubkey()
    mint = Keypair.from_seed(hashlib.sha256(b"mint" + bytes([i])).digest()).pubkey()
    tp = tok if i % 2 == 0 else tok22
    a, _ = Pubkey.find_program_address([bytes(owner), bytes(tp), bytes(mint)], ata_prog)
    atas.append((str(owner), str(mint), str(tp), str(a)))
out.append("struct AtaVector { std::string_view owner, mint, token_program, address; };")
out.append("constexpr auto ata_vectors = std::to_array<AtaVector>({")
out += ['    {"%s", "%s", "%s", "%s"},' % t for t in atas]
out.append("});")

# seed("name") keypairs: ed25519 seed = sha256("tide:seed:" + name)
names = ["alice", "bob", "admin", "usd", ""]
out.append("constexpr auto seed_vectors = std::to_array<std::pair<std::string_view, std::string_view>>({")
for n in names:
    kp = Keypair.from_seed(hashlib.sha256(b"tide:seed:" + n.encode()).digest())
    out.append('    {"%s", "%s"},' % (n, str(kp.pubkey())))
out.append("});")

# Signed legacy transaction, byte-exact
def kp(n): return Keypair.from_seed(hashlib.sha256(b"tide:seed:" + n.encode()).digest())
alice, bob, carol = kp("alice"), kp("bob"), kp("carol")
bh = Hash(hashlib.sha256(b"blockhash").digest())
ix1 = Instruction(programs[1], bytes([3]) + (250000).to_bytes(8, "little"),
                  [AccountMeta(bob.pubkey(), False, True), AccountMeta(carol.pubkey(), False, True),
                   AccountMeta(alice.pubkey(), True, False)])
memo = Pubkey.from_string("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr")
ix2 = Instruction(memo, b"hello", [AccountMeta(carol.pubkey(), True, False)])
msg = Message.new_with_blockhash([ix1, ix2], alice.pubkey(), bh)
tx = Transaction([alice, carol], msg, bh)
out.append('constexpr std::string_view tx_vector_hex = "%s";' % bytes(tx).hex())
print("\n".join(out))
*/
// clang-format on
