// The OpenSSL wrappers against published vectors, and the device proof.
#include "base/bytes.h"
#include "crypto/crypto_device_key.h"
#include "crypto/crypto_primitives.h"

#include <QtTest/QtTest>

namespace {

[[nodiscard]] QByteArray Hex(const char *hex) {
	return QByteArray::fromHex(hex);
}

} // namespace

class TestCrypto final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void sha256AndHmac();
	void incrementalSha256();
	void noiseHkdf();
	void x25519Rfc7748();
	void chachaPolyRfc8439();
	void ed25519Rfc8032();
	void deviceProof();
	void randomBytes();

};

void TestCrypto::sha256AndHmac() {
	QCOMPARE(Crypto::Sha256(QByteArray("abc")).toHex(), QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	QCOMPARE(Crypto::Sha256(QByteArray()).toHex(), QByteArray("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	// RFC 4231, test case 1 and 2.
	QCOMPARE(
		Crypto::HmacSha256(QByteArray(20, char(0x0b)), QByteArray("Hi There")).toHex(),
		QByteArray("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
	QCOMPARE(
		Crypto::HmacSha256(QByteArray("Jefe"), QByteArray("what do ya want for nothing?")).toHex(),
		QByteArray("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
	// An empty key and empty data are valid inputs.
	QCOMPARE(
		Crypto::HmacSha256(QByteArray(), QByteArray()).toHex(),
		QByteArray("b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"));
}

void TestCrypto::incrementalSha256() {
	auto hasher = Crypto::Sha256Hasher();
	hasher.feed(QByteArray("a"));
	hasher.feed(QByteArray());
	hasher.feed("bc", 2);
	QCOMPARE(hasher.result(), Crypto::Sha256(QByteArray("abc")));
}

void TestCrypto::noiseHkdf() {
	// The construction itself (HMAC chain), checked against a direct computation.
	const auto chainingKey = QByteArray(32, char(1));
	const auto input = QByteArray("input key material");
	const auto temp = Crypto::HmacSha256(chainingKey, input);
	const auto one = Crypto::HmacSha256(temp, QByteArray(1, char(1)));
	const auto two = Crypto::HmacSha256(temp, one + QByteArray(1, char(2)));
	const auto three = Crypto::HmacSha256(temp, two + QByteArray(1, char(3)));
	const auto pair = Crypto::NoiseHkdf(chainingKey, input, 2);
	QCOMPARE(pair.first, one);
	QCOMPARE(pair.second, two);
	QVERIFY(pair.third.isEmpty());
	QCOMPARE(Crypto::NoiseHkdf(chainingKey, input, 3).third, three);
}

void TestCrypto::x25519Rfc7748() {
	const auto alice = Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
	const auto bob = Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
	const auto alicePublic = Crypto::X25519PublicFromPrivate(alice);
	const auto bobPublic = Crypto::X25519PublicFromPrivate(bob);
	QCOMPARE(alicePublic.toHex(), QByteArray("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
	QCOMPARE(bobPublic.toHex(), QByteArray("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
	const auto shared = Hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
	QCOMPARE(Crypto::X25519Shared(alice, bobPublic), shared);
	QCOMPARE(Crypto::X25519Shared(bob, alicePublic), shared);
	QVERIFY(Crypto::X25519Shared(alice, QByteArray(31, 'x')).isEmpty());
	QVERIFY(Crypto::X25519PublicFromPrivate(QByteArray(5, 'x')).isEmpty());
	// The all-zero point is rejected (a contributory-behaviour check OpenSSL performs).
	QVERIFY(Crypto::X25519Shared(alice, QByteArray(32, char(0))).isEmpty());
	QCOMPARE(Crypto::X25519GeneratePrivate().size(), 32);
}

void TestCrypto::chachaPolyRfc8439() {
	const auto key = Hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
	const auto nonce = Hex("070000004041424344454647");
	const auto aad = Hex("50515253c0c1c2c3c4c5c6c7");
	const auto plaintext = QByteArray("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
	const auto expected = Hex(
		"d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
		"3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
		"92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
		"3ff4def08e4b7a9de576d26586cec64b6116"
		"1ae10b594f09e26a7e902ecbd0600691");
	const auto sealed = Crypto::ChaChaPolyEncrypt(key, nonce, aad, plaintext);
	QCOMPARE(sealed, expected);
	QCOMPARE(Crypto::ChaChaPolyDecrypt(key, nonce, aad, sealed).value_or(QByteArray()), plaintext);

	auto tampered = sealed;
	tampered[3] = char(tampered[3] ^ 0x20);
	QVERIFY(!Crypto::ChaChaPolyDecrypt(key, nonce, aad, tampered).has_value());
	QVERIFY(!Crypto::ChaChaPolyDecrypt(key, nonce, QByteArray("other"), sealed).has_value());
	QVERIFY(!Crypto::ChaChaPolyDecrypt(key, nonce, aad, sealed.left(10)).has_value());

	// An empty plaintext is just the tag, and opens to an empty (non-null) array.
	const auto tagOnly = Crypto::ChaChaPolyEncrypt(key, nonce, QByteArray(), QByteArray());
	QCOMPARE(tagOnly.size(), Crypto::kTagSize);
	const auto opened = Crypto::ChaChaPolyDecrypt(key, nonce, QByteArray(), tagOnly);
	QVERIFY(opened.has_value());
	QVERIFY(opened->isEmpty() && !opened->isNull());
	QVERIFY(Crypto::ChaChaPolyEncrypt(key.left(31), nonce, aad, plaintext).isEmpty());
}

void TestCrypto::ed25519Rfc8032() {
	// Test 1 (empty message) and test 2 (one byte).
	const auto secret1 = Hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	const auto public1 = Hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	const auto signature1 = Hex(
		"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
		"5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
	QCOMPARE(Crypto::Ed25519PublicFromPrivate(secret1), public1);
	QCOMPARE(Crypto::Ed25519Sign(secret1, QByteArray()), signature1);
	QVERIFY(Crypto::Ed25519Verify(public1, QByteArray(), signature1));

	const auto secret2 = Hex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
	const auto public2 = Hex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
	const auto signature2 = Hex(
		"92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
		"085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
	QCOMPARE(Crypto::Ed25519PublicFromPrivate(secret2), public2);
	QCOMPARE(Crypto::Ed25519Sign(secret2, Hex("72")), signature2);
	QVERIFY(Crypto::Ed25519Verify(public2, Hex("72"), signature2));
	QVERIFY(!Crypto::Ed25519Verify(public2, Hex("73"), signature2));
	QVERIFY(!Crypto::Ed25519Verify(public1, Hex("72"), signature2));
	QVERIFY(!Crypto::Ed25519Verify(public2, Hex("72"), signature2.left(63)));
}

void TestCrypto::deviceProof() {
	QCOMPARE(Crypto::DeviceKey::Type(), u"ed25519"_s);
	QCOMPARE(
		Crypto::DeviceKey::ProofMessage(u"refresh"_s, QByteArray("\x01\x02", 2)),
		QByteArray("eleza/device-proof/v1\0refresh\0\x01\x02", 32));

	QVERIFY(!Crypto::DeviceKey().valid());
	QVERIFY(Crypto::DeviceKey().sign(u"refresh"_s, QByteArray("n")).isEmpty());
	QVERIFY(!Crypto::DeviceKey::FromPrivate(QByteArray(3, 'x')).valid());

	const auto key = Crypto::DeviceKey::Generate();
	QVERIFY(key.valid());
	QCOMPARE(key.publicKey().size(), 32);
	QCOMPARE(base::bytes::fromBase64(key.publicKeyBase64()), key.publicKey());
	const auto nonce = Crypto::RandomBytes(16);
	const auto signature = key.sign(u"ws_resume"_s, nonce);
	QCOMPARE(signature.size(), 64);
	QVERIFY(Crypto::DeviceKey::Verify(key.publicKey(), u"ws_resume"_s, nonce, signature));
	// The purpose and the nonce are both bound.
	QVERIFY(!Crypto::DeviceKey::Verify(key.publicKey(), u"refresh"_s, nonce, signature));
	QVERIFY(!Crypto::DeviceKey::Verify(key.publicKey(), u"ws_resume"_s, nonce + "x", signature));
	// Restoring from the stored private part gives the same identity.
	const auto restored = Crypto::DeviceKey::FromPrivate(key.privateKey());
	QCOMPARE(restored.publicKey(), key.publicKey());
	QCOMPARE(restored.sign(u"ws_resume"_s, nonce), signature); // deterministic
	QVERIFY(Crypto::DeviceKey::Generate().publicKey() != key.publicKey());
}

void TestCrypto::randomBytes() {
	QCOMPARE(Crypto::RandomBytes(0).size(), 0);
	QCOMPARE(Crypto::RandomBytes(48).size(), 48);
	QVERIFY(Crypto::RandomBytes(32) != Crypto::RandomBytes(32));
}

QTEST_GUILESS_MAIN(TestCrypto)

#include "test_crypto.moc"
